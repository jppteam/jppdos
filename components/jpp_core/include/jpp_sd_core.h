#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPP_SD_STATUS_OK = 0,
    JPP_SD_STATUS_INVALID_ARGUMENT,
    JPP_SD_STATUS_UNAVAILABLE,
} jpp_sd_status_t;

typedef enum {
    JPP_SD_EVENT_NONE = 0,
    JPP_SD_EVENT_INSERTED,
    JPP_SD_EVENT_REMOVED,
} jpp_sd_event_t;

typedef enum {
    JPP_SD_UNAVAILABLE_NONE = 0,
    JPP_SD_UNAVAILABLE_MISSING_SD,
    JPP_SD_UNAVAILABLE_CONFIG_MISSING,
} jpp_sd_unavailable_reason_t;

typedef struct {
    bool has_sck;
    bool has_mosi;
    bool has_miso;
    bool has_cs;
    int sck;
    int mosi;
    int miso;
    int cs;
} jpp_sd_config_t;

typedef struct {
    bool ready;
    bool mounted;
    int sck;
    int mosi;
    int miso;
    int cs;
    jpp_sd_unavailable_reason_t unavailable_reason;
} jpp_sd_state_t;

void jpp_sd_config_defaults(jpp_sd_config_t *config);
jpp_sd_status_t jpp_sd_state_init(jpp_sd_state_t *state, const jpp_sd_config_t *config, bool mounted);
jpp_sd_status_t jpp_sd_handle_card_present(jpp_sd_state_t *state, bool present, jpp_sd_event_t *event);
const char *jpp_sd_status_name(jpp_sd_status_t status);
const char *jpp_sd_event_name(jpp_sd_event_t event);
const char *jpp_sd_unavailable_reason_name(jpp_sd_unavailable_reason_t reason);

#ifdef __cplusplus
}
#endif
