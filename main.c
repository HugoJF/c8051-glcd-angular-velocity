#include <stdio.h>
#include <stdlib.h>
#include "config.c"
#include "def_pinos.h"
#include "fonte.c"

// BIG8051
// Timer0 -> delay
// Timer1 -> medição velocidade angular
// Timer2 -> UART0
// Timer3 -> base de tempo (4ms)
// Timer4 -> emulação do sinal do encoder

// Macros to make code more readable
#define true (1)
#define false (0)
#define bool __bit

// CLI Operations
#define OP_START 'i'
#define OP_STOP 'p'
#define OP_CONFIGURE 'r'

// GLCD defines
#define CS1 P2_0
#define CS2 P2_1
#define RS P2_2
#define RW P2_3
#define E P2_4
#define RST P2_5
#define DB P4

#define CO 0
#define DA 1

#define LEFT 0
#define RIGHT 1

#define NOP4() NOP();NOP();NOP();NOP()
#define NOP8() NOP4();NOP4();
#define NOP12() NOP4();NOP8();

// Last key that was pressed by serial input
volatile unsigned char keypress = '\0';

// Used during pulse measures
volatile float pulse_duration = 0;

// Last measured pulse
float measured_duration = 0;

// Time reference
volatile float time = 0;

// Input buffer
unsigned char buffer[4];

// Current RPM setting
unsigned int rpm = 100;

// Are we measuring pulse widths?
bool measuring = 0;


// only used for UX stuff
void delay(unsigned int ms) {
    TMOD |= 0x01;
    TMOD &= ~0x02;

    while (ms-- > 0) {
        TR0 = 0;
        TF0 = 0;
        TL0 = 0x58;
        TH0 = 0x9e;
        TR0 = 1;
        while (TF0 == 0);
    }
}


// read a single char from serial terminal
unsigned char read_char(void) {
    unsigned char caught;

    while (keypress == '\0');

    // use temp variable to allow this function to reset the 'keypress' flag
    caught = keypress;
    keypress = '\0';

    return caught;
}


// handles key presses emulation
void int_serial(void) __interrupt INTERRUPT_UART0 {
    if (RI0 == 1) {
        keypress = SBUF0;
        RI0 = 0;
    }
}


// reads from GLCD
unsigned char glcd_read(bool cd, bool cs) {
    unsigned char byte;

    RW = 1;
    CS1 = cs;
    CS2 = !cs;
    RS = cd;
    NOP4();
    E = 1;
    NOP8();
    SFRPAGE = CONFIG_PAGE;
    byte = DB;
    SFRPAGE = LEGACY_PAGE;
    NOP4();
    E = 0;
    NOP12();

    return byte;
}

// writes data to GLCD 
void glcd_write(unsigned char byte, bool cd, bool cs) {
    while (glcd_read(CO, cs) & 0x80);

    RW = 0;
    CS1 = cs;
    CS2 = !cs;
    RS = cd;
    SFRPAGE = CONFIG_PAGE;
    DB = byte;
    SFRPAGE = LEGACY_PAGE;
    NOP4();
    E = 1;
    NOP12();
    E = 0;
    SFRPAGE = CONFIG_PAGE;
    DB = 0xff;
    SFRPAGE = LEGACY_PAGE;
    NOP12();
}


// initializes glcd
void glcd_init(void) {
    E = 0;
    RST = 1;
    CS1 = 1;
    CS2 = 1;
    SFRPAGE = CONFIG_PAGE;
    DB = 0xff;
    SFRPAGE = LEGACY_PAGE;
    while (glcd_read(CO, LEFT) & 0x10);
    while (glcd_read(CO, RIGHT) & 0x10);
    glcd_write(0x3f, CO, LEFT);
    glcd_write(0x3f, CO, RIGHT);
    glcd_write(0x40, CO, LEFT);
    glcd_write(0xb8, CO, LEFT);
    glcd_write(0xc0, CO, LEFT);
    glcd_write(0x40, CO, RIGHT);
    glcd_write(0xb8, CO, RIGHT);
    glcd_write(0xc0, CO, RIGHT);
}


// measures a single pulse duration
float measure_pulse(void) {
    unsigned timer;

    // wait for 0-1 transition
    while (P1_0 == 1);

    // Reset duration variable
    pulse_duration = 0;
    // Reset external interrupt flag
    IE1 = 0;
    // Reset Timer1 counters
    TL1 = 0;
    TH1 = 0;
    // Turn Timer1 on
    TR1 = 1;
    // Wait for 1-0 transition
    while (IE1 == 0);
    // Turn Timer1 off
    TR1 = 0;
    // Clean Timer1 overflow flag
    TF1 = 0;

    // Calculate Timer1 remainder
    timer = (unsigned int) TH1 * 256 + (unsigned int) TL1;

    // Calculate pulse duration
    pulse_duration = pulse_duration + (float) timer / 25000000;

    return pulse_duration;
}


// keep track of time
void int_time(void) __interrupt INTERRUPT_TIMER3 {
    TF3 = 0;

    // only count time if we are measuring
    if (measuring) {
        time += 0.004;
    }
}

// simulates track-wheel - this is not connected via crossbar!
// connecting T3/T4 disabled interrupts for whatever reason
void int_simu(void) __interrupt INTERRUPT_TIMER4 {
    TF4 = 0;

    P1_2 = !P1_2;
}


// reset timer1 when measuring pulse width so we have a wider range of time
void int_pulse(void) __interrupt INTERRUPT_TIMER1 {
    // Reset overflow flag
    TF1 = 0;

    pulse_duration += 0.00262144; // Timer1 has a 2.6ms overflow

    // Interrupt if pulse duration is greater than 1 second
    if (pulse_duration > 1.0f) {
        IE1 = 1;
    }
}


// prints configuration screen
void print_configure() {
    unsigned int val = atoi(buffer);

    printf_fast_f("\x01 === CONFIG ===");
    printf_fast_f("\x03 Digite nova RPM");
    printf_fast_f("\x05 > RPM: %u", val);
}


// prints home screen
void print_home() {
    float rpm ;

    printf_fast_f("\x01=== PROVA 3 ===");
    printf_fast_f("\x03 > i: iniciar");
    printf_fast_f("\x04 > p: parar");
    printf_fast_f("\x05 > r: setar RPM");

    // skip RPM part of screen if not measuring
    if (!measuring) {
        return;
    }

    // convert to RPM
    rpm = 0.03 / measured_duration;

    // display user-friendly message about RPM
    if (rpm < 20) {
        printf_fast_f("\x07 RPM = <=20.0");
    } else if (rpm > 900) {
        printf_fast_f("\x07 RPM = >=900.0");
    } else {
        printf_fast_f("\x07 RPM = %f", rpm);
    }
}

void glcd_set_page(unsigned char pag, bool cs) {
    glcd_write(0xB8 + (pag & 0x07), CO, cs);
}

void glcd_set_y(unsigned char y, bool cs) {
    glcd_write(0x40 + (y & 0x3F), CO, cs);
}

void glcd_clear_side(bool cs) {
    unsigned char linha, i;

    for (linha = 0; linha < 8; linha++) {
        glcd_set_page(linha, cs);
        glcd_set_y(0, cs);
        for (i = 0; i < 64; i++) {
            glcd_write(0x00, DA, cs);
        }
    }
}

void glcd_clear() {
    glcd_clear_side(LEFT);
    glcd_clear_side(RIGHT);
}

// handles printf
void putchar(unsigned char c) {
    unsigned char index;
    bool side;
    static unsigned char char_counter = 0;

    // Control char
    if (c < 9) {
        glcd_set_page(c - 1, LEFT);
        glcd_set_page(c - 1, RIGHT);
        glcd_set_y(0, LEFT);
        glcd_set_y(0, RIGHT);
        char_counter = 0;
    } else {
        if (char_counter < 8) { 
            side = LEFT;
        } else {
            side = RIGHT;
        }

        index = c - 32;
        char_counter++;

        glcd_write(fonte[index][0], DA, side);
        glcd_write(fonte[index][1], DA, side);
        glcd_write(fonte[index][2], DA, side);
        glcd_write(fonte[index][3], DA, side);
        glcd_write(fonte[index][4], DA, side);
        glcd_write(0x00, DA, side);
        glcd_write(0x00, DA, side);
        glcd_write(0x00, DA, side);
    }
}


// configure Timer4 to emulate RPM
void set_timer4(unsigned int rpm) {
    // calculate pulse width
    float pw = 0.03f / (float) rpm;
    // calculate how many counts to timer overflow
    unsigned int counts = pw * 25000000.0f;

    // pw to timer sets
    SFRPAGE = TMR4_PAGE;
    // subtract counts from 2**16-1 since it's a 16bit counter 
    RCAP4H = (unsigned short) ((65535 - counts) >> 8);
    RCAP4L = (unsigned short) (65535 - counts);
    SFRPAGE = LEGACY_PAGE;
}


// runs config wizard
void configuration_wizard(void) {
    unsigned char i = 0;

    // make sure buffer is not filled with trash
    for (i = 0; i < 4; ++i) {
        buffer[i] = '\0';
    }

    // avoid any hanging chars in screen
    glcd_clear();

    // update screen
    print_configure();

    // read 3 inputs from user terminal
    for (i = 0; i < 3; ++i) {
        buffer[i] = read_char();
        print_configure();
    }

    // make sure buffer is null terminated
    buffer[sizeof(buffer) - 1] = '\0';


    printf_fast_f("\x07 Confirme com E");

    // wait for enter
    if (read_char() != 'e') {
        glcd_clear();
        printf_fast_f("\x01 > ERRO !");
        printf_fast_f("\x03 > Tecle E para");
        printf_fast_f("\x04 > confirmar");

        // hold text on screen
        delay(5000);

        return;
    }

    // convert to integer
    rpm = atoi(buffer);

    // clamp
    if (rpm < 18) {
        rpm = 18;
    }

    if (rpm > 902) {
        rpm = 902;
    }

    set_timer4(rpm);
}


// that function
void main(void) {
    unsigned int i = 0;
    Init_Device();
    SFRPAGE = LEGACY_PAGE;

    glcd_init();
    glcd_clear();

    // init timer here why not
    set_timer4(rpm);

    while (true) {
        // start measuring if OP_START was caught
        if (keypress == OP_START) {
            keypress = '\0';
            measuring = true;
            glcd_clear();
        }

        // stop measuring if OP_CONFIGURE was caught
        if (keypress == OP_STOP) {
            keypress = '\0';
            measuring = false;
            glcd_clear();
        }

        // start configuring procedure if OP_CONFIGURE was caught
        if (keypress == OP_CONFIGURE) {
            keypress = '\0';
            configuration_wizard();
            glcd_clear();
        }

        // measure each second if it's enabled
        if (measuring && time > 1) {
            time = 0;
            measured_duration = measure_pulse();
        }

        // update screen
        print_home();
    }
}
    