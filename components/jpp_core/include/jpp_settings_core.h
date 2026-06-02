#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPP_SETTINGS_SOURCE_UNKNOWN = 0,
    JPP_SETTINGS_SOURCE_DEFAULTS,
    JPP_SETTINGS_SOURCE_STORED,
} jpp_settings_source_t;

typedef enum {
    JPP_SETTINGS_PAYLOAD_MISSING = 0,
    JPP_SETTINGS_PAYLOAD_VALID,
    JPP_SETTINGS_PAYLOAD_MIGRATION_REQUIRED,
    JPP_SETTINGS_PAYLOAD_CORRUPT,
} jpp_settings_payload_status_t;

typedef enum {
    JPP_SETTINGS_STATUS_OK = 0,
    JPP_SETTINGS_STATUS_INVALID_ARGUMENT,
    JPP_SETTINGS_STATUS_INVALID_STATE,
} jpp_settings_status_t;

typedef struct {
    bool main_exists;
    bool temp_exists;
    jpp_settings_payload_status_t payload_status;
} jpp_settings_storage_probe_t;

typedef struct {
    jpp_settings_source_t source;
    bool used_defaults_marker;
    bool used_stored_marker;
    bool migrated_marker;
    bool recovered_marker;
    bool corrupt_reset_marker;
} jpp_settings_load_result_t;

void jpp_settings_load_result_init(jpp_settings_load_result_t *result);

jpp_settings_status_t jpp_settings_classify(
    const jpp_settings_storage_probe_t *probe,
    jpp_settings_load_result_t *result
);

const char *jpp_settings_source_name(jpp_settings_source_t source);
const char *jpp_settings_payload_status_name(jpp_settings_payload_status_t status);
const char *jpp_settings_status_name(jpp_settings_status_t status);

#ifdef __cplusplus
}
#endif
