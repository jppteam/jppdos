#include "../include/jpp_sd_core.h"

#include <string.h>

static bool jpp_sd_config_present(const jpp_sd_config_t *config)
{
    return config != NULL &&
           config->has_sck && config->sck >= 0 &&
           config->has_mosi && config->mosi >= 0 &&
           config->has_miso && config->miso >= 0 &&
           config->has_cs && config->cs >= 0;
}

void jpp_sd_config_defaults(jpp_sd_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->has_sck = true;
    config->has_mosi = true;
    config->has_miso = true;
    config->has_cs = true;
    config->sck = 14;
    config->mosi = 15;
    config->miso = 18;
    config->cs = 7;
}

jpp_sd_status_t jpp_sd_state_init(jpp_sd_state_t *state, const jpp_sd_config_t *config, bool mounted)
{
    jpp_sd_config_t defaults;
    const jpp_sd_config_t *source;

    if (state == NULL) {
        return JPP_SD_STATUS_INVALID_ARGUMENT;
    }
    jpp_sd_config_defaults(&defaults);
    source = config == NULL ? &defaults : config;
    memset(state, 0, sizeof(*state));
    if (!jpp_sd_config_present(source)) {
        state->unavailable_reason = JPP_SD_UNAVAILABLE_CONFIG_MISSING;
        return JPP_SD_STATUS_UNAVAILABLE;
    }
    state->sck = source->sck;
    state->mosi = source->mosi;
    state->miso = source->miso;
    state->cs = source->cs;
    state->mounted = mounted;
    if (!mounted) {
        state->unavailable_reason = JPP_SD_UNAVAILABLE_MISSING_SD;
        return JPP_SD_STATUS_UNAVAILABLE;
    }
    state->ready = true;
    state->unavailable_reason = JPP_SD_UNAVAILABLE_NONE;
    return JPP_SD_STATUS_OK;
}

jpp_sd_status_t jpp_sd_handle_card_present(jpp_sd_state_t *state, bool present, jpp_sd_event_t *event)
{
    bool was_mounted;

    if (state == NULL) {
        return JPP_SD_STATUS_INVALID_ARGUMENT;
    }
    was_mounted = state->mounted;
    if (event != NULL) {
        *event = JPP_SD_EVENT_NONE;
    }
    state->mounted = present;
    state->ready = present;
    state->unavailable_reason = present ? JPP_SD_UNAVAILABLE_NONE : JPP_SD_UNAVAILABLE_MISSING_SD;
    if (event != NULL && was_mounted != present) {
        *event = present ? JPP_SD_EVENT_INSERTED : JPP_SD_EVENT_REMOVED;
    }
    return present ? JPP_SD_STATUS_OK : JPP_SD_STATUS_UNAVAILABLE;
}

const char *jpp_sd_status_name(jpp_sd_status_t status)
{
    switch (status) {
    case JPP_SD_STATUS_OK:
        return "OK";
    case JPP_SD_STATUS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case JPP_SD_STATUS_UNAVAILABLE:
        return "UNAVAILABLE";
    }
    return "UNKNOWN";
}

const char *jpp_sd_event_name(jpp_sd_event_t event)
{
    switch (event) {
    case JPP_SD_EVENT_NONE:
        return "none";
    case JPP_SD_EVENT_INSERTED:
        return "inserted";
    case JPP_SD_EVENT_REMOVED:
        return "removed";
    }
    return "unknown";
}

const char *jpp_sd_unavailable_reason_name(jpp_sd_unavailable_reason_t reason)
{
    switch (reason) {
    case JPP_SD_UNAVAILABLE_NONE:
        return "none";
    case JPP_SD_UNAVAILABLE_MISSING_SD:
        return "missing_sd";
    case JPP_SD_UNAVAILABLE_CONFIG_MISSING:
        return "config_missing";
    }
    return "unknown";
}
