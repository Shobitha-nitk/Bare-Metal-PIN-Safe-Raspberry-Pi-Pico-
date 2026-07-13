#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/pwm.h"
#include "hardware/i2c.h"

// Pin Definitions
#define RED_LED_PIN 15
#define GRN_LED_PIN 14
#define BUZZER_PIN 16

// I2C LCD Configuration (Standard 16x2 via PCF8574)
#define I2C_PORT i2c0
#define LCD_ADDR 0x27
// NOTE: moved off GP4/GP5 - those pins are already used by ROW_PINS below.
// Sharing pins between the keypad rows and I2C caused the LCD (and/or the
// keypad) to silently stop working because init_peripherals() and lcd_init()
// were fighting over the same GPIOs' function select.
#define SDA_PIN 20
#define SCL_PIN 21

const uint ROW_PINS[4] = {2, 3, 4, 5};
const uint COL_PINS[4] = {6, 7, 8, 9};

const char KEY_MAP[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

typedef enum {
    STATE_SLEEP,
    STATE_LOCKED,
    STATE_ENTERING_PIN,
    STATE_UNLOCKED,
    STATE_LOCKOUT
} SafeState;

const char MASTER_PIN[4] = {'1', '2', '3', '4'};
#define MAX_WRONG_ATTEMPTS 3

volatile uint32_t system_ticks = 0;
volatile bool wake_requested = false;

bool repeating_timer_callback(struct repeating_timer *t) {
    system_ticks++;
    return true;
}

// --- I2C LCD Low-Level Driver (PCF8574 backpack, 4-bit HD44780 protocol) ---
// PCF8574 -> HD44780 pin mapping used by virtually all "IIC/I2C 1602" boards:
//   P0 = RS, P1 = RW, P2 = EN, P3 = Backlight, P4-P7 = D4-D7
#define LCD_BACKLIGHT 0x08
#define LCD_ENABLE    0x04
#define LCD_RW        0x02
#define LCD_RS        0x01

static void lcd_i2c_write(uint8_t data) {
    i2c_write_blocking(I2C_PORT, LCD_ADDR, &data, 1, false);
}

static void lcd_pulse_enable(uint8_t data) {
    sleep_us(600);
    lcd_i2c_write(data | LCD_ENABLE);
    sleep_us(600);
    lcd_i2c_write(data & ~LCD_ENABLE);
    sleep_us(600);
}

// Sends ONE 4-bit nibble (top nibble of the byte passed in). This is what the
// HD44780 datasheet reset sequence requires - the controller starts up in
// 8-bit mode, so each reset step must be a single nibble pulse. Sending a
// full two-nibble byte here (as lcd_send_byte does) injects an extra,
// unwanted pulse and breaks the handshake - which is why the screen lit up
// but never showed any characters.
static void lcd_write4bits(uint8_t nibble) {
    uint8_t data = (nibble & 0xF0) | LCD_BACKLIGHT;
    lcd_i2c_write(data);
    lcd_pulse_enable(data);
}

// mode = 0 for command, LCD_RS for data. Sends val as two 4-bit nibbles.
// Only valid AFTER the LCD has been switched into 4-bit mode.
void lcd_send_byte(uint8_t val, uint8_t mode) {
    uint8_t high_nibble = (val & 0xF0) | mode | LCD_BACKLIGHT;
    uint8_t low_nibble  = ((val << 4) & 0xF0) | mode | LCD_BACKLIGHT;

    lcd_i2c_write(high_nibble);
    lcd_pulse_enable(high_nibble);

    lcd_i2c_write(low_nibble);
    lcd_pulse_enable(low_nibble);
}

void lcd_init() {
    i2c_init(I2C_PORT, 100 * 1000);
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SDA_PIN);
    gpio_pull_up(SCL_PIN);

    sleep_ms(50); // wait for LCD power-on

    // HD44780 4-bit init sequence (per datasheet) - single nibble pulses only
    lcd_write4bits(0x03 << 4);
    sleep_ms(5);
    lcd_write4bits(0x03 << 4);
    sleep_us(150);
    lcd_write4bits(0x03 << 4);
    sleep_us(150);
    lcd_write4bits(0x02 << 4); // switch to 4-bit mode

    // From here on the controller is in 4-bit mode, so full-byte commands
    // via lcd_send_byte() (two nibbles each) are correct.
    lcd_send_byte(0x28, 0); // function set: 4-bit, 2 line, 5x8 font
    lcd_send_byte(0x0C, 0); // display on, cursor off, blink off
    lcd_send_byte(0x06, 0); // entry mode: increment, no shift
    lcd_send_byte(0x01, 0); // clear display
    sleep_ms(2);
}

void lcd_set_cursor(int line, int col) {
    static const uint8_t row_offsets[2] = {0x00, 0x40};
    if (line > 1) line = 1;
    if (col > 15) col = 15;
    lcd_send_byte(0x80 | (col + row_offsets[line]), 0);
}

void lcd_print(const char *str) {
    while (*str) {
        lcd_send_byte((uint8_t)(*str++), LCD_RS);
    }
}

void lcd_clear() {
    lcd_send_byte(0x01, 0);
    sleep_ms(2);
}

// --- Buzzer PWM Functions ---
void init_audio() {
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_enabled(slice_num, false);
}

void play_tone(uint frequency, uint duration_ms) {
    uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
    uint32_t clock = 125000000; // 125 MHz default Pico clock
    uint32_t divider = 16;
    uint32_t wrap = (clock / (divider * frequency)) - 1;

    pwm_set_clkdiv(slice_num, divider);
    pwm_set_wrap(slice_num, wrap);
    pwm_set_gpio_level(BUZZER_PIN, wrap / 2); // 50% duty cycle
    pwm_set_enabled(slice_num, true);

    sleep_ms(duration_ms);
    pwm_set_enabled(slice_num, false);
}

// --- GPIO Interrupt for Wake Up ---
void keypad_irq_callback(uint gpio, uint32_t events) {
    wake_requested = true;
}

void setup_wake_interrupts(bool enable) {
    for (int i = 0; i < 4; i++) {
        gpio_set_irq_enabled_with_callback(COL_PINS[i], GPIO_IRQ_EDGE_RISE, enable, &keypad_irq_callback);
    }
}

// Drives ALL rows high before sleeping. scan_keypad() is never called while
// asleep, so unless every row is already high, a key press can't pull any
// column pin high - meaning the wake interrupt would never fire and the
// system would stay stuck in STATE_SLEEP forever (this was the bug causing
// "keypad and LEDs don't work": the code never left STATE_SLEEP).
void enter_sleep_mode() {
    for (int i = 0; i < 4; i++) {
        gpio_put(ROW_PINS[i], 1);
    }
    setup_wake_interrupts(true);
}

// Disables the raw edge interrupts and puts rows back to their normal idle
// LOW state so scan_keypad()'s one-row-at-a-time scanning works correctly
// (leaving all rows high would let a single key press register on every
// row simultaneously - ghost key presses).
void exit_sleep_mode() {
    setup_wake_interrupts(false);
    for (int i = 0; i < 4; i++) {
        gpio_put(ROW_PINS[i], 0);
    }
}

void init_peripherals() {
    gpio_init(RED_LED_PIN);
    gpio_set_dir(RED_LED_PIN, GPIO_OUT);
    gpio_init(GRN_LED_PIN);
    gpio_set_dir(GRN_LED_PIN, GPIO_OUT);

    for (int i = 0; i < 4; i++) {
        gpio_init(ROW_PINS[i]);
        gpio_set_dir(ROW_PINS[i], GPIO_OUT);
        gpio_put(ROW_PINS[i], 0);

        gpio_init(COL_PINS[i]);
        gpio_set_dir(COL_PINS[i], GPIO_IN);
        gpio_pull_down(COL_PINS[i]);
    }

    init_audio();
    lcd_init();
}

char scan_keypad() {
    for (int r = 0; r < 4; r++) {
        gpio_put(ROW_PINS[r], 1);
        for (int c = 0; c < 4; c++) {
            if (gpio_get(COL_PINS[c])) {
                gpio_put(ROW_PINS[r], 0);
                play_tone(2000, 30); // Audible key click feedback
                return KEY_MAP[r][c];
            }
        }
        gpio_put(ROW_PINS[r], 0);
    }
    return '\0';
}

int main() {
    stdio_init_all();
    init_peripherals();

    struct repeating_timer timer;
    add_repeating_timer_ms(-1, repeating_timer_callback, NULL, &timer);

    SafeState current_state = STATE_SLEEP;
    SafeState prev_state_for_display = STATE_SLEEP;
    uint32_t last_scan_time = 0;
    uint32_t state_timer = 0;
    char last_key = '\0';

    char user_entered_pin[4] = {0};
    int pin_index = 0;
    int wrong_attempts = 0;

    printf("--- BARE METAL SECURITY SYSTEM ONLINE ---\n");
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print("Press * to Start");
    enter_sleep_mode();

    while (true) {
        char current_key = '\0';

        // True only on the single loop pass where we just switched into a
        // new state - used to guard one-time LCD redraws so we don't call
        // lcd_clear()/lcd_print() on every single loop iteration (which was
        // causing the "Access Granted" flicker: lcd_clear() was firing
        // thousands of times a second while sitting in STATE_UNLOCKED).
        bool entered_state = (current_state != prev_state_for_display);
        prev_state_for_display = current_state;

        // Handle input throttling
        if (current_state != STATE_SLEEP && (system_ticks - last_scan_time >= 50)) {
            last_scan_time = system_ticks;
            char scanned = scan_keypad();
            if (scanned != last_key) {
                last_key = scanned;
                current_key = scanned;
            }
        }

        // Check for interrupt wake trigger
        if (wake_requested) {
            wake_requested = false;
            if (current_state == STATE_SLEEP) {
                exit_sleep_mode(); // Disable raw interrupts and reset rows low for normal scanning
                current_state = STATE_LOCKED;
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_print("System Locked");
                play_tone(1500, 100);
            }
        }

        switch (current_state) {
            case STATE_SLEEP:
                gpio_put(RED_LED_PIN, 0);
                gpio_put(GRN_LED_PIN, 0);
                // Low power idle state, waiting for interrupt
                sleep_ms(100);
                break;

            case STATE_LOCKED:
                gpio_put(RED_LED_PIN, 1);
                gpio_put(GRN_LED_PIN, 0);

                lcd_set_cursor(1, 0);
                lcd_print("Enter PIN: ____");

                if (current_key != '\0' && current_key >= '0' && current_key <= '9') {
                    user_entered_pin[0] = current_key;
                    pin_index = 1;
                    state_timer = system_ticks;
                    current_state = STATE_ENTERING_PIN;
                } else if (current_key == '*') {
                    // Manual reset or refresh back to sleep if needed
                    state_timer = system_ticks;
                }

                // Inactivity check to drop back to sleep after 10 seconds of idle locked
                if (system_ticks - state_timer > 10000 && state_timer != 0) {
                    lcd_clear();
                    lcd_print("Press * to Start");
                    enter_sleep_mode();
                    current_state = STATE_SLEEP;
                }
                break;

            case STATE_ENTERING_PIN:
                gpio_put(RED_LED_PIN, (system_ticks % 300 < 150));

                // Update LCD with masked characters
                lcd_set_cursor(1, 11);
                char mask[5] = "____";
                for (int i = 0; i < pin_index; i++) mask[i] = '*';
                lcd_print(mask);

                if (system_ticks - state_timer > 7000) {
                    printf("Inactivity Timeout.\n");
                    memset(user_entered_pin, 0, sizeof(user_entered_pin));
                    pin_index = 0;
                    current_state = STATE_LOCKED;
                }

                if (current_key != '\0') {
                    state_timer = system_ticks;

                    if (current_key == '*') {
                        memset(user_entered_pin, 0, sizeof(user_entered_pin));
                        pin_index = 0;
                        current_state = STATE_LOCKED;
                    }
                    else if (pin_index < 4 && current_key >= '0' && current_key <= '9') {
                        user_entered_pin[pin_index] = current_key;
                        pin_index++;

                        if (pin_index == 4) {
                            // Show the 4th star and give the user a moment
                            // to actually see the completed PIN on screen
                            // before we verify it and move on.
                            lcd_set_cursor(1, 11);
                            lcd_print("****");
                            sleep_ms(500);

                            bool pin_correct = true;
                            for (int i = 0; i < 4; i++) {
                                if (user_entered_pin[i] != MASTER_PIN[i]) {
                                    pin_correct = false;
                                }
                            }

                            // Clear sensitive RAM data immediately
                            memset(user_entered_pin, 0, sizeof(user_entered_pin));
                            pin_index = 0;

                            if (pin_correct) {
                                play_tone(2500, 200);
                                play_tone(3000, 200);
                                wrong_attempts = 0;
                                state_timer = system_ticks;
                                current_state = STATE_UNLOCKED;
                            } else {
                                wrong_attempts++;
                                play_tone(500, 400); // Error buzz

                                lcd_clear();
                                lcd_set_cursor(0, 0);
                                lcd_print("Wrong PIN!");
                                lcd_set_cursor(1, 0);
                                if (wrong_attempts >= MAX_WRONG_ATTEMPTS) {
                                    lcd_print("Locking out...");
                                } else {
                                    char attempts_msg[17];
                                    snprintf(attempts_msg, sizeof(attempts_msg), "Attempts left: %d", MAX_WRONG_ATTEMPTS - wrong_attempts);
                                    lcd_print(attempts_msg);
                                }
                                sleep_ms(1500); // hold the message so it's actually readable

                                if (wrong_attempts >= MAX_WRONG_ATTEMPTS) {
                                    state_timer = system_ticks;
                                    current_state = STATE_LOCKOUT;
                                } else {
                                    current_state = STATE_LOCKED;
                                }
                            }
                        }
                    }
                }
                break;

            case STATE_UNLOCKED:
                gpio_put(RED_LED_PIN, 0);
                gpio_put(GRN_LED_PIN, 1);
                if (entered_state) {
                    // Only redraw once, on entry - calling lcd_clear() every
                    // loop iteration (thousands of times/sec) was what
                    // caused the "Access Granted" flicker.
                    lcd_clear();
                    lcd_set_cursor(0, 0);
                    lcd_print("Access Granted");
                }

                if (system_ticks - state_timer > 5000) {
                    lcd_clear();
                    current_state = STATE_LOCKED;
                }
                break;

            case STATE_LOCKOUT:
                gpio_put(GRN_LED_PIN, 0);
                gpio_put(RED_LED_PIN, (system_ticks % 100 < 50));

                if (entered_state) {
                    lcd_clear();
                    lcd_set_cursor(0, 0);
                    lcd_print("SECURITY LOCKOUT");
                }

                if (system_ticks - state_timer > 10000) {
                    wrong_attempts = 0;
                    current_state = STATE_LOCKED;
                }
                break;
        }
    }
}
