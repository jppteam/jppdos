#include "../include/jpp_settings_core.h"

#include <string.h>

void jpp_settings_load_result_init(jpp_settings_load_result_t *result)
{
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->source = JPP_SETTINGS_SOURCE_UNKNOWN;
}

jpp_settings_status_t jpp_settings_classify(
    const jpp_settings_storage_probe_t *probe,
    jpp_settings_load_result_t *result
)
{
    bool effective_main_exists;

    if (probe == NULL || result == NULL) {
        return JPP_SETTINGS_STATUS_INVALID_ARGUMENT;
    }

    jpp_settings_load_result_init(result);
    effective_main_exists = probe->main_exists || probe->temp_exists;

    if (!effective_main_exists) {
        if (probe->payload_status != JPP_SETTINGS_PAYLOAD_MISSING) {
            return JPP_SETTINGS_STATUS_INVALID_STATE;
        }
        result->source = JPP_SETTINGS_SOURCE_DEFAULTS;
        result->used_defaults_marker = true;
        return JPP_SETTINGS_STATUS_OK;
    }

    result->recovered_marker = !probe->main_exists && probe->temp_exists;

    switch (probe->payload_status) {
    case JPP_SETTINGS_PAYLOAD_VALID:
        result->source = JPP_SETTINGS_SOURCE_STORED;
        result->used_stored_marker = true;
        return JPP_SETTINGS_STATUS_OK;
    case JPP_SETTINGS_PAYLOAD_CORRUPT:
        result->source = JPP_SETTINGS_SOURCE_DEFAULTS;
        result->used_defaults_marker = true;
        result->corrupt_reset_marker = true;
        return JPP_SETTINGS_STATUS_OK;
    case JPP_SETTINGS_PAYLOAD_MISSING:
    default:
        return JPP_SETTINGS_STATUS_INVALID_STATE;
    }
}

const char *jpp_settings_source_name(jpp_settings_source_t source)
{
    switch (source) {
    case JPP_SETTINGS_SOURCE_DEFAULTS:
        return "defaults";
    case JPP_SETTINGS_SOURCE_STORED:
        return "stored";
    case JPP_SETTINGS_SOURCE_UNKNOWN:
    default:
        return "unknown";
    }
}

const char *jpp_settings_payload_status_name(jpp_settings_payload_status_t status)
{
    switch (status) {
    case JPP_SETTINGS_PAYLOAD_MISSING:
        return "missing";
    case JPP_SETTINGS_PAYLOAD_VALID:
        return "valid";
    case JPP_SETTINGS_PAYLOAD_CORRUPT:
        return "corrupt";
    }
    return "unknown";
}

const char *jpp_settings_status_name(jpp_settings_status_t status)
{
    switch (status) {
    case JPP_SETTINGS_STATUS_OK:
        return "OK";
    case JPP_SETTINGS_STATUS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case JPP_SETTINGS_STATUS_INVALID_STATE:
        return "INVALID_STATE";
    }
    return "UNKNOWN";
}
