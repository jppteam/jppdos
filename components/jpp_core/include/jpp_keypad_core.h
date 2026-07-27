#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default fallback values used when jpp_keypad_config_t fields are unset. */
#define JPP_KEYPAD_DEFAULT_HYSTERESIS_UV      20000
#define JPP_KEYPAD_DEFAULT_DEBOUNCE_SAMPLES   2
#define JPP_KEYPAD_DEFAULT_LONG_PRESS_MS      700
#define JPP_KEYPAD_DEFAULT_POLL_INTERVAL_MS   100
#define JPP_KEYPAD_DEFAULT_REPEAT_DELAY_MS    500
#define JPP_KEYPAD_DEFAULT_REPEAT_INTERVAL_MS 500
#define JPP_KEYPAD_DEFAULT_DOUBLE_CLICK_MS    300

/* Selects how CENTER triggers "Back": holding for long_press_ms (default,
   zero-latency OK), or two short clicks within double_click_ms (defers OK
   until the window passes with no second click). Mutually exclusive. */
typedef enum {
    JPP_KEYPAD_BACK_GESTURE_HOLD = 0,
    JPP_KEYPAD_BACK_GESTURE_DOUBLE_CLICK,
} jpp_keypad_back_gesture_t;

typedef enum {
    JPP_KEYPAD_KIND_NO_EVENT = 0,
    JPP_KEYPAD_KIND_PRESS,
    JPP_KEYPAD_KIND_RELEASE,
    JPP_KEYPAD_KIND_REPEAT,
    JPP_KEYPAD_KIND_CENTER_SHORT,
    JPP_KEYPAD_KIND_CENTER_LONG,
    JPP_KEYPAD_KIND_CENTER_DOUBLE,
} jpp_keypad_event_kind_t;

typedef struct {
    const char *key;
    int center_uv;
    int tolerance_uv;
    bool repeatable;
} jpp_keypad_band_t;

typedef struct {
    bool enabled;
    int calibration_uv;
    int hysteresis_uv;
    int debounce_samples;
    int long_press_ms;
    int poll_interval_ms;
    bool repeat_enabled;
    int repeat_delay_ms;
    int repeat_interval_ms;
    jpp_keypad_back_gesture_t back_gesture;
    int double_click_ms;
    const jpp_keypad_band_t *bands;
    size_t band_count;
} jpp_keypad_config_t;

typedef struct {
    jpp_keypad_event_kind_t kind;
    const char *key;
    const char *mapped;
    int duration_ms;
} jpp_keypad_event_t;

typedef struct {
    bool ready;
    bool enabled;
    int calibration_uv;
    int hysteresis_uv;
    int debounce_samples;
    int long_press_ms;
    int poll_interval_ms;
    bool repeat_enabled;
    int repeat_delay_ms;
    int repeat_interval_ms;
    const char *stable_key;
    const char *candidate_key;
    int candidate_count;
    int press_started_ms;
    int last_repeat_ms;
    bool center_long_emitted;
    size_t sample_index;
    /* Double-click back-gesture: a deferred short click waiting to see if a
       second click follows within double_click_ms. Survives across the idle
       gap between two separate press/release cycles, unlike press_started_ms
       above which jpp_keypad_reset_hold_state() clears on every release. */
    bool ok_pending;
    int pending_release_ms;
} jpp_keypad_state_t;

void jpp_keypad_state_init(jpp_keypad_state_t *state, const jpp_keypad_config_t *config);
const char *jpp_keypad_event_kind_name(jpp_keypad_event_kind_t kind);
int jpp_keypad_poll(
    jpp_keypad_state_t *state,
    const jpp_keypad_config_t *config,
    int sample_uv,
    bool sample_present,
    jpp_keypad_event_t *events,
    size_t event_capacity,
    size_t *event_count
);

#ifdef __cplusplus
}
#endif
