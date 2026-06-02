#include "../include/jpp_network_core.h"

#include <string.h>

static int positive_or_default(int value, int default_value)
{
    return value > 0 ? value : default_value;
}

void jpp_network_config_defaults(jpp_network_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->enabled = true;
    config->connected = false;
    config->auto_connect = false;
    config->ssid = NULL;
    config->password = "";
    config->connect_timeout_s = 30;
    config->connect_poll_interval_ms = 250;
    config->reconnect_initial_backoff_s = 1;
    config->reconnect_max_backoff_s = 30;
    config->reconnect_max_attempts = 5;
}

jpp_network_status_t jpp_network_state_init(
    jpp_network_state_t *state,
    const jpp_network_config_t *config,
    bool wifi_available
)
{
    jpp_network_config_t defaults;
    const jpp_network_config_t *source;

    if (state == NULL) {
        return JPP_NETWORK_STATUS_INVALID_ARGUMENT;
    }
    jpp_network_config_defaults(&defaults);
    source = config == NULL ? &defaults : config;
    memset(state, 0, sizeof(*state));
    state->enabled = source->enabled;
    state->connected = source->connected;
    state->auto_connect = source->auto_connect;
    state->ssid = source->ssid;
    state->password = source->password != NULL ? source->password : "";
    state->connect_timeout_s = positive_or_default(source->connect_timeout_s, 30);
    state->connect_poll_interval_ms = positive_or_default(source->connect_poll_interval_ms, 250);
    state->reconnect_initial_backoff_s = positive_or_default(source->reconnect_initial_backoff_s, 1);
    state->reconnect_max_backoff_s = positive_or_default(source->reconnect_max_backoff_s, 30);
    state->reconnect_max_attempts = positive_or_default(source->reconnect_max_attempts, 5);
    if (!state->enabled) {
        state->connected = false;
        state->unavailable_reason = JPP_NETWORK_UNAVAILABLE_DISABLED;
        return JPP_NETWORK_STATUS_UNAVAILABLE;
    }
    if (!wifi_available) {
        state->connected = false;
        state->unavailable_reason = JPP_NETWORK_UNAVAILABLE_WIFI_UNAVAILABLE;
        return JPP_NETWORK_STATUS_UNAVAILABLE;
    }
    state->ready = true;
    state->unavailable_reason = JPP_NETWORK_UNAVAILABLE_NONE;
    return JPP_NETWORK_STATUS_OK;
}

jpp_network_status_t jpp_network_configure(
    jpp_network_state_t *state,
    const char *ssid,
    const char *password,
    bool connect,
    bool connect_succeeds
)
{
    if (state == NULL) {
        return JPP_NETWORK_STATUS_INVALID_ARGUMENT;
    }
    if (!state->ready) {
        return JPP_NETWORK_STATUS_UNAVAILABLE;
    }
    state->ssid = ssid != NULL && ssid[0] != '\0' ? ssid : NULL;
    state->password = password != NULL ? password : "";
    state->auto_connect = connect && state->ssid != NULL;
    if (!state->auto_connect) {
        state->connected = false;
        return JPP_NETWORK_STATUS_OK;
    }
    state->connected = connect_succeeds;
    return connect_succeeds ? JPP_NETWORK_STATUS_OK : JPP_NETWORK_STATUS_CONNECT_FAILED;
}

const char *jpp_network_status_name(jpp_network_status_t status)
{
    switch (status) {
    case JPP_NETWORK_STATUS_OK:
        return "OK";
    case JPP_NETWORK_STATUS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case JPP_NETWORK_STATUS_UNAVAILABLE:
        return "UNAVAILABLE";
    case JPP_NETWORK_STATUS_CONNECT_FAILED:
        return "CONNECT_FAILED";
    }
    return "UNKNOWN";
}

const char *jpp_network_unavailable_reason_name(jpp_network_unavailable_reason_t reason)
{
    switch (reason) {
    case JPP_NETWORK_UNAVAILABLE_NONE:
        return "none";
    case JPP_NETWORK_UNAVAILABLE_DISABLED:
        return "disabled";
    case JPP_NETWORK_UNAVAILABLE_WIFI_UNAVAILABLE:
        return "wifi_unavailable";
    }
    return "unknown";
}
