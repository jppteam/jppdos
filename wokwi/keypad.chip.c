/*
 * keypad.chip.c — Wokwi custom chip: resistor-ladder keypad ADC bridge
 *
 * Accepts five active-LOW digital button inputs and drives one analog output
 * pin (OUT) with the voltage the real resistor-ladder circuit would produce.
 * Wire OUT to GPIO 2 (ADC1_CH2) so the unmodified firmware sees the correct
 * ADC reading via jpp_keypad_poll's band-matching logic.
 *
 * Voltage table (VCC = 3.3 V, 10 kΩ pull-down, series resistor per key):
 *
 *   UP     270 kΩ  → 3.3 × 10/(10+270) = 0.1179 V  (band centre 0.12 V ±0.04 V)
 *   DOWN   100 kΩ  → 3.3 × 10/(10+100) = 0.3000 V  (band centre 0.30 V ±0.04 V)
 *   LEFT    56 kΩ  → 3.3 × 10/(10+ 56) = 0.5000 V  (band centre 0.50 V ±0.04 V)
 *   RIGHT   39 kΩ  → 3.3 × 10/(10+ 39) = 0.6735 V  (band centre 0.70 V ±0.04 V)
 *   CENTER  27 kΩ  → 3.3 × 10/(10+ 27) = 0.8919 V  (band centre 0.90 V ±0.04 V)
 *   (none)                               = 0.0000 V  (below all bands → idle)
 *
 * When multiple buttons are pressed simultaneously the lowest-index button
 * wins (matches real resistor-ladder behaviour where the lowest parallel
 * resistance dominates).
 */

#include "wokwi-api.h"

#define BTN_COUNT 5

static const float  BTN_VOLTAGES[BTN_COUNT] = { 0.1179f, 0.3000f, 0.5000f, 0.6735f, 0.8919f };
static const char  *BTN_NAMES[BTN_COUNT]    = { "UP", "DOWN", "LEFT", "RIGHT", "CENTER" };

static pin_t g_btn[BTN_COUNT];
static pin_t g_out;

/* Recompute and drive OUT whenever any button changes state. */
static void on_btn_change(void *user_data, pin_t pin, uint32_t value) {
    float v = 0.0f;
    for (int i = 0; i < BTN_COUNT; i++) {
        if (pin_read(g_btn[i]) == 0) { /* active LOW */
            v = BTN_VOLTAGES[i];
            break;
        }
    }
    pin_dac_write(g_out, v);
}

void chip_init(void) {
    pin_watch_config_t watch_cfg = {
        .edge       = BOTH,
        .pin_change = on_btn_change,
        .user_data  = NULL,
    };

    for (int i = 0; i < BTN_COUNT; i++) {
        g_btn[i] = pin_init(BTN_NAMES[i], INPUT_PULLUP);
        pin_watch(g_btn[i], &watch_cfg);
    }

    g_out = pin_init("OUT", ANALOG);
    pin_dac_write(g_out, 0.0f);
}
