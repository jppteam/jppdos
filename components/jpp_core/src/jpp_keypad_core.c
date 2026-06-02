#include "../include/jpp_keypad_core.h"

#include <string.h>

static const jpp_keypad_band_t JPP_KEYPAD_DEFAULT_BANDS[] = {
    {"UP", 120000, 40000, true},
    {"DOWN", 300000, 40000, true},
    {"LEFT", 500000, 40000, true},
    {"RIGHT", 700000, 40000, true},
    {"CENTER", 900000, 40000, false},
};

static const jpp_keypad_band_t *jpp_keypad_config_bands(const jpp_keypad_config_t *config, size_t *band_count)
{
    if (config != NULL && config->bands != NULL && config->band_count > 0u) {
        *band_count = config->band_count;
        return config->bands;
    }
    *band_count = sizeof(JPP_KEYPAD_DEFAULT_BANDS) / sizeof(JPP_KEYPAD_DEFAULT_BANDS[0]);
    return JPP_KEYPAD_DEFAULT_BANDS;
}

static int jpp_keypad_config_hysteresis_uv(const jpp_keypad_config_t *config)
{
    if (config == NULL) {
        return JPP_KEYPAD_DEFAULT_HYSTERESIS_UV;
    }
    return config->hysteresis_uv > 0 ? config->hysteresis_uv : 0;
}

static int jpp_keypad_config_debounce_samples(const jpp_keypad_config_t *config)
{
    if (config == NULL || config->debounce_samples < 1) {
        return JPP_KEYPAD_DEFAULT_DEBOUNCE_SAMPLES;
    }
    return config->debounce_samples;
}

static int jpp_keypad_config_long_press_ms(const jpp_keypad_config_t *config)
{
    if (config == NULL || config->long_press_ms < 1) {
        return JPP_KEYPAD_DEFAULT_LONG_PRESS_MS;
    }
    return config->long_press_ms;
}

static int jpp_keypad_config_poll_interval_ms(const jpp_keypad_config_t *config)
{
    if (config == NULL || config->poll_interval_ms < 1) {
        return JPP_KEYPAD_DEFAULT_POLL_INTERVAL_MS;
    }
    return config->poll_interval_ms;
}

static int jpp_keypad_config_repeat_delay_ms(const jpp_keypad_config_t *config)
{
    if (config == NULL || config->repeat_delay_ms < 1) {
        return JPP_KEYPAD_DEFAULT_REPEAT_DELAY_MS;
    }
    return config->repeat_delay_ms;
}

static int jpp_keypad_config_repeat_interval_ms(const jpp_keypad_config_t *config)
{
    if (config == NULL || config->repeat_interval_ms < 1) {
        return JPP_KEYPAD_DEFAULT_REPEAT_INTERVAL_MS;
    }
    return config->repeat_interval_ms;
}

static const jpp_keypad_band_t *jpp_keypad_band_by_key(const jpp_keypad_config_t *config, const char *key)
{
    size_t band_count = 0u;
    const jpp_keypad_band_t *bands = jpp_keypad_config_bands(config, &band_count);
    size_t index;

    if (key == NULL) {
        return NULL;
    }
    for (index = 0u; index < band_count; ++index) {
        if (bands[index].key != NULL && strcmp(bands[index].key, key) == 0) {
            return &bands[index];
        }
    }
    return NULL;
}

static const jpp_keypad_band_t *jpp_keypad_match_band(
    const jpp_keypad_state_t *state,
    const jpp_keypad_config_t *config,
    int adjusted_uv
)
{
    size_t band_count = 0u;
    const jpp_keypad_band_t *bands = jpp_keypad_config_bands(config, &band_count);
    const jpp_keypad_band_t *best_match = NULL;
    int best_distance = 0;
    size_t match_count = 0u;
    size_t index;

    for (index = 0u; index < band_count; ++index) {
        int tolerance_uv = bands[index].tolerance_uv > 0 ? bands[index].tolerance_uv : 1;
        int distance = adjusted_uv - bands[index].center_uv;
        if (distance < 0) {
            distance = -distance;
        }
        if (distance <= tolerance_uv) {
            if (match_count == 0u || distance < best_distance) {
                best_match = &bands[index];
                best_distance = distance;
            }
            match_count += 1u;
        }
    }
    if (match_count > 0u) {
        return best_match;
    }
    if (state == NULL || state->stable_key == NULL) {
        return NULL;
    }
    {
        const jpp_keypad_band_t *active_band = jpp_keypad_band_by_key(config, state->stable_key);
        int tolerance_uv;
        int center_uv;
        int distance;

        if (active_band == NULL) {
            return NULL;
        }
        tolerance_uv = active_band->tolerance_uv > 0 ? active_band->tolerance_uv : 1;
        center_uv = active_band->center_uv;
        distance = adjusted_uv - center_uv;
        if (distance < 0) {
            distance = -distance;
        }
        if (distance <= tolerance_uv + jpp_keypad_config_hysteresis_uv(config)) {
            return active_band;
        }
    }
    return NULL;
}

static int jpp_keypad_emit_repeat_if_needed(
    jpp_keypad_state_t *state,
    const jpp_keypad_config_t *config,
    const char *current_key,
    int now_ms,
    jpp_keypad_event_t *events,
    size_t event_capacity,
    size_t *event_count
)
{
    const jpp_keypad_band_t *band;

    if (state == NULL || current_key == NULL || events == NULL || event_count == NULL) {
        return 0;
    }
    if (config != NULL && !config->repeat_enabled) {
        return 0;
    }
    band = jpp_keypad_band_by_key(config, current_key);
    if (band == NULL || !band->repeatable || state->press_started_ms < 0) {
        return 0;
    }
    if (now_ms - state->press_started_ms < jpp_keypad_config_repeat_delay_ms(config)) {
        return 0;
    }
    if (state->last_repeat_ms < 0) {
        state->last_repeat_ms = state->press_started_ms + jpp_keypad_config_repeat_delay_ms(config);
    }
    if (now_ms - state->last_repeat_ms < jpp_keypad_config_repeat_interval_ms(config)) {
        return 0;
    }
    if (*event_count >= event_capacity) {
        return -1;
    }
    state->last_repeat_ms = now_ms;
    events[*event_count].kind = JPP_KEYPAD_KIND_REPEAT;
    events[*event_count].key = current_key;
    events[*event_count].mapped = current_key;
    events[*event_count].duration_ms = 0;
    *event_count += 1u;
    return 0;
}

static int jpp_keypad_finalize_release(
    jpp_keypad_state_t *state,
    const jpp_keypad_config_t *config,
    const char *previous_key,
    int now_ms,
    jpp_keypad_event_t *events,
    size_t event_capacity,
    size_t *event_count
)
{
    int duration_ms;

    if (state == NULL || previous_key == NULL || events == NULL || event_count == NULL) {
        return 0;
    }
    if (state->press_started_ms < 0) {
        return 0;
    }
    duration_ms = now_ms - state->press_started_ms;
    if (duration_ms < 0) {
        duration_ms = 0;
    }
    if (strcmp(previous_key, "CENTER") == 0) {
        if (duration_ms >= jpp_keypad_config_long_press_ms(config) && !state->center_long_emitted) {
            if (*event_count >= event_capacity) {
                return -1;
            }
            events[*event_count].kind = JPP_KEYPAD_KIND_CENTER_LONG;
            events[*event_count].key = previous_key;
            events[*event_count].mapped = "BACK";
            events[*event_count].duration_ms = duration_ms;
            *event_count += 1u;
        } else if (duration_ms < jpp_keypad_config_long_press_ms(config)) {
            if (*event_count >= event_capacity) {
                return -1;
            }
            events[*event_count].kind = JPP_KEYPAD_KIND_CENTER_SHORT;
            events[*event_count].key = previous_key;
            events[*event_count].mapped = "OK";
            events[*event_count].duration_ms = duration_ms;
            *event_count += 1u;
        }
        return 0;
    }
    if (*event_count >= event_capacity) {
        return -1;
    }
    events[*event_count].kind = JPP_KEYPAD_KIND_RELEASE;
    events[*event_count].key = previous_key;
    events[*event_count].mapped = previous_key;
    events[*event_count].duration_ms = duration_ms;
    *event_count += 1u;
    return 0;
}

static void jpp_keypad_reset_hold_state(jpp_keypad_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->press_started_ms = -1;
    state->last_repeat_ms = -1;
    state->center_long_emitted = false;
}

void jpp_keypad_state_init(jpp_keypad_state_t *state, const jpp_keypad_config_t *config)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->enabled = config == NULL ? true : config->enabled;
    state->calibration_uv = config == NULL ? 0 : config->calibration_uv;
    state->hysteresis_uv = config == NULL ? 20000 : config->hysteresis_uv;
    state->debounce_samples = config == NULL ? 2 : config->debounce_samples;
    state->long_press_ms = config == NULL ? 700 : config->long_press_ms;
    state->poll_interval_ms = config == NULL ? 100 : config->poll_interval_ms;
    state->repeat_enabled = config == NULL ? true : config->repeat_enabled;
    state->repeat_delay_ms = config == NULL ? 450 : config->repeat_delay_ms;
    state->repeat_interval_ms = config == NULL ? 150 : config->repeat_interval_ms;
    state->press_started_ms = -1;
    state->last_repeat_ms = -1;
    state->ready = state->enabled;
}

const char *jpp_keypad_event_kind_name(jpp_keypad_event_kind_t kind)
{
    switch (kind) {
    case JPP_KEYPAD_KIND_NO_EVENT:
        return "NO_EVENT";
    case JPP_KEYPAD_KIND_PRESS:
        return "PRESS";
    case JPP_KEYPAD_KIND_RELEASE:
        return "RELEASE";
    case JPP_KEYPAD_KIND_REPEAT:
        return "REPEAT";
    case JPP_KEYPAD_KIND_CENTER_SHORT:
        return "CENTER_SHORT";
    case JPP_KEYPAD_KIND_CENTER_LONG:
        return "CENTER_LONG";
    }
    return "UNKNOWN";
}

int jpp_keypad_poll(
    jpp_keypad_state_t *state,
    const jpp_keypad_config_t *config,
    int sample_uv,
    bool sample_present,
    jpp_keypad_event_t *events,
    size_t event_capacity,
    size_t *event_count
)
{
    const jpp_keypad_band_t *candidate_band;
    const char *candidate_key;
    int adjusted_uv;
    const char *previous_key;
    int now_ms;

    if (state == NULL || events == NULL || event_count == NULL) {
        return -1;
    }
    *event_count = 0u;
    if (!state->enabled || (config != NULL && !config->enabled)) {
        return 0;
    }
    now_ms = (int)state->sample_index * jpp_keypad_config_poll_interval_ms(config);
    if (!sample_present) {
        if (state->stable_key != NULL) {
            previous_key = state->stable_key;
            state->stable_key = NULL;
            state->candidate_key = NULL;
            state->candidate_count = 0;
            if (jpp_keypad_finalize_release(state, config, previous_key, now_ms, events, event_capacity, event_count) != 0) {
                return -1;
            }
            jpp_keypad_reset_hold_state(state);
        }
        state->sample_index += 1u;
        return 0;
    }
    adjusted_uv = sample_uv + state->calibration_uv;
    candidate_band = jpp_keypad_match_band(state, config, adjusted_uv);
    candidate_key = candidate_band != NULL ? candidate_band->key : NULL;
    if (candidate_key == state->stable_key) {
        if (candidate_key != NULL) {
            if (state->press_started_ms < 0) {
                state->press_started_ms = now_ms;
            }
            if (strcmp(candidate_key, "CENTER") == 0) {
                int duration_ms = now_ms - state->press_started_ms;
                if (duration_ms >= jpp_keypad_config_long_press_ms(config) && !state->center_long_emitted) {
                    if (*event_count >= event_capacity) {
                        return -1;
                    }
                    state->center_long_emitted = true;
                    events[*event_count].kind = JPP_KEYPAD_KIND_CENTER_LONG;
                    events[*event_count].key = candidate_key;
                    events[*event_count].mapped = "BACK";
                    events[*event_count].duration_ms = duration_ms;
                    *event_count += 1u;
                } else if (state->center_long_emitted &&
                           now_ms - state->press_started_ms >=
                               jpp_keypad_config_long_press_ms(config) +
                               jpp_keypad_config_repeat_delay_ms(config) &&
                           now_ms - state->last_repeat_ms >= jpp_keypad_config_repeat_interval_ms(config)) {
                    if (*event_count >= event_capacity) {
                        return -1;
                    }
                    state->last_repeat_ms = now_ms;
                    events[*event_count].kind = JPP_KEYPAD_KIND_REPEAT;
                    events[*event_count].key = candidate_key;
                    events[*event_count].mapped = "BACK";
                    events[*event_count].duration_ms = 0;
                    *event_count += 1u;
                }
            } else if (jpp_keypad_emit_repeat_if_needed(state, config, candidate_key, now_ms, events, event_capacity, event_count) != 0) {
                return -1;
            }
        }
        state->sample_index += 1u;
        return 0;
    }
    if (candidate_key == state->candidate_key) {
        state->candidate_count += 1;
        if (state->candidate_count < jpp_keypad_config_debounce_samples(config)) {
            state->sample_index += 1u;
            return 0;
        }
    } else {
        state->candidate_key = candidate_key;
        state->candidate_count = 1;
        state->sample_index += 1u;
        return 0;
    }
    previous_key = state->stable_key;
    state->stable_key = candidate_key;
    state->candidate_count = 0;
    if (candidate_key == NULL) {
        if (previous_key != NULL) {
            if (jpp_keypad_finalize_release(state, config, previous_key, now_ms, events, event_capacity, event_count) != 0) {
                return -1;
            }
        }
        state->candidate_key = NULL;
        jpp_keypad_reset_hold_state(state);
        state->sample_index += 1u;
        return 0;
    }
    state->press_started_ms = now_ms;
    state->last_repeat_ms = now_ms;
    state->center_long_emitted = false;
    if (strcmp(candidate_key, "CENTER") != 0) {
        if (*event_count >= event_capacity) {
            return -1;
        }
        events[*event_count].kind = JPP_KEYPAD_KIND_PRESS;
        events[*event_count].key = candidate_key;
        events[*event_count].mapped = candidate_key;
        events[*event_count].duration_ms = 0;
        *event_count += 1u;
    }
    state->candidate_key = candidate_key;
    state->sample_index += 1u;
    return 0;
}
