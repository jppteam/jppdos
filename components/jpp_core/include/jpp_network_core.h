#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPP_NETWORK_STATUS_OK = 0,
    JPP_NETWORK_STATUS_INVALID_ARGUMENT,
    JPP_NETWORK_STATUS_UNAVAILABLE,
    JPP_NETWORK_STATUS_CONNECT_FAILED,
} jpp_network_status_t;

typedef enum {
    JPP_NETWORK_UNAVAILABLE_NONE = 0,
    JPP_NETWORK_UNAVAILABLE_DISABLED,
    JPP_NETWORK_UNAVAILABLE_WIFI_UNAVAILABLE,
} jpp_network_unavailable_reason_t;

typedef struct {
    bool enabled;
    bool connected;
    bool auto_connect;
    const char *ssid;
    const char *password;
    int connect_timeout_s;
    int connect_poll_interval_ms;
    int reconnect_initial_backoff_s;
    int reconnect_max_backoff_s;
    int reconnect_max_attempts;
} jpp_network_config_t;

typedef struct {
    bool ready;
    bool enabled;
    bool connected;
    bool auto_connect;
    const char *ssid;
    const char *password;
    int connect_timeout_s;
    int connect_poll_interval_ms;
    int reconnect_initial_backoff_s;
    int reconnect_max_backoff_s;
    int reconnect_max_attempts;
    jpp_network_unavailable_reason_t unavailable_reason;
} jpp_network_state_t;

void jpp_network_config_defaults(jpp_network_config_t *config);
jpp_network_status_t jpp_network_state_init(
    jpp_network_state_t *state,
    const jpp_network_config_t *config,
    bool wifi_available
);
jpp_network_status_t jpp_network_configure(
    jpp_network_state_t *state,
    const char *ssid,
    const char *password,
    bool connect,
    bool connect_succeeds
);
const char *jpp_network_status_name(jpp_network_status_t status);
const char *jpp_network_unavailable_reason_name(jpp_network_unavailable_reason_t reason);

#ifdef __cplusplus
}
#endif
