#include "../include/jpp_oled_core.h"

#include <string.h>

static bool jpp_oled_config_present(const jpp_oled_config_t *config)
{
    return config != NULL &&
           config->has_width && config->width > 0 &&
           config->has_height && config->height > 0 &&
           config->has_i2c_id && config->i2c_id >= 0 &&
           config->has_sda_pin && config->sda_pin >= 0 &&
           config->has_scl_pin && config->scl_pin >= 0 &&
           config->has_freq && config->freq > 0 &&
           config->has_address && config->address > 0u;
}

void jpp_oled_config_defaults(jpp_oled_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->enabled = true;
    config->has_width = true;
    config->has_height = true;
    config->has_i2c_id = true;
    config->has_sda_pin = true;
    config->has_scl_pin = true;
    config->has_freq = true;
    config->has_address = true;
    config->width = JPP_OLED_DEFAULT_WIDTH;
    config->height = JPP_OLED_DEFAULT_HEIGHT;
    config->i2c_id = JPP_OLED_DEFAULT_I2C_ID;
    config->sda_pin = JPP_OLED_DEFAULT_SDA_PIN;
    config->scl_pin = JPP_OLED_DEFAULT_SCL_PIN;
    config->freq = JPP_OLED_DEFAULT_FREQ_HZ;
    config->address = JPP_OLED_DEFAULT_ADDRESS;
}

jpp_oled_status_t jpp_oled_state_init(
    jpp_oled_state_t *state,
    const jpp_oled_config_t *config,
    bool i2c_available
)
{
    if (state == NULL) {
        return JPP_OLED_STATUS_INVALID_ARGUMENT;
    }
    memset(state, 0, sizeof(*state));
    if (config == NULL) {
        state->unavailable_reason = JPP_OLED_UNAVAILABLE_CONFIG_MISSING;
        return JPP_OLED_STATUS_UNAVAILABLE;
    }
    state->enabled = config->enabled;
    if (!config->enabled) {
        state->unavailable_reason = JPP_OLED_UNAVAILABLE_DISABLED;
        return JPP_OLED_STATUS_UNAVAILABLE;
    }
    if (!jpp_oled_config_present(config)) {
        state->unavailable_reason = JPP_OLED_UNAVAILABLE_CONFIG_MISSING;
        return JPP_OLED_STATUS_UNAVAILABLE;
    }
    state->width = config->width;
    state->height = config->height;
    state->i2c_id = config->i2c_id;
    state->sda_pin = config->sda_pin;
    state->scl_pin = config->scl_pin;
    state->freq = config->freq;
    state->address = config->address;
    state->frame_bytes = ((size_t)state->width * (size_t)state->height) / 8u;
    if (!i2c_available) {
        state->unavailable_reason = JPP_OLED_UNAVAILABLE_I2C_UNAVAILABLE;
        return JPP_OLED_STATUS_UNAVAILABLE;
    }
    state->available = true;
    state->unavailable_reason = JPP_OLED_UNAVAILABLE_NONE;
    return JPP_OLED_STATUS_OK;
}

jpp_oled_status_t jpp_oled_frame_bytes(const jpp_oled_state_t *state, size_t *frame_bytes)
{
    if (state == NULL || frame_bytes == NULL) {
        return JPP_OLED_STATUS_INVALID_ARGUMENT;
    }
    *frame_bytes = state->frame_bytes;
    return state->available ? JPP_OLED_STATUS_OK : JPP_OLED_STATUS_UNAVAILABLE;
}

const char *jpp_oled_status_name(jpp_oled_status_t status)
{
    switch (status) {
    case JPP_OLED_STATUS_OK:
        return "OK";
    case JPP_OLED_STATUS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case JPP_OLED_STATUS_UNAVAILABLE:
        return "UNAVAILABLE";
    }
    return "UNKNOWN";
}

const char *jpp_oled_unavailable_reason_name(jpp_oled_unavailable_reason_t reason)
{
    switch (reason) {
    case JPP_OLED_UNAVAILABLE_NONE:
        return "none";
    case JPP_OLED_UNAVAILABLE_DISABLED:
        return "disabled";
    case JPP_OLED_UNAVAILABLE_CONFIG_MISSING:
        return "config_missing";
    case JPP_OLED_UNAVAILABLE_I2C_UNAVAILABLE:
        return "i2c_unavailable";
    }
    return "unknown";
}
