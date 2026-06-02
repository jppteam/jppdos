#include "../include/jpp_boot_core.h"

#include <string.h>

static const jpp_boot_event_t JPP_BOOT_EVENT_ORDER[JPP_BOOT_EVENT_CAPACITY] = {
    JPP_BOOT_EVENT_BOOT_START,
    JPP_BOOT_EVENT_STORAGE_READY,
    JPP_BOOT_EVENT_SETTINGS_READY,
    JPP_BOOT_EVENT_SD_MOUNT_READY,
    JPP_BOOT_EVENT_RUNTIME_PATHS_READY,
    JPP_BOOT_EVENT_SERVICES_READY,
    JPP_BOOT_EVENT_DISCOVERY_READY,
    JPP_BOOT_EVENT_SYSTEM_READY,
};

static jpp_boot_status_t jpp_boot_append_event(jpp_boot_context_t *context, jpp_boot_event_t event)
{
    if (context == NULL) {
        return JPP_BOOT_STATUS_INVALID_ARGUMENT;
    }
    if (context->system_ready) {
        return JPP_BOOT_STATUS_FINALIZED;
    }
    if (context->event_count >= JPP_BOOT_EVENT_CAPACITY) {
        return JPP_BOOT_STATUS_OVERFLOW;
    }
    if (JPP_BOOT_EVENT_ORDER[context->event_count] != event) {
        return JPP_BOOT_STATUS_OUT_OF_ORDER;
    }
    context->events[context->event_count] = event;
    context->event_count += 1u;
    return JPP_BOOT_STATUS_OK;
}

void jpp_boot_context_init(jpp_boot_context_t *context)
{
    if (context == NULL) {
        return;
    }
    memset(context, 0, sizeof(*context));
    context->boot_mode = JPP_BOOT_MODE_UNKNOWN;
    context->boot_reason = "pending";
    context->sd_mount_reason = "pending";
    jpp_settings_load_result_init(&context->settings_state);
    (void)jpp_boot_append_event(context, JPP_BOOT_EVENT_BOOT_START);
}

jpp_boot_status_t jpp_boot_note_storage_ready(jpp_boot_context_t *context)
{
    jpp_boot_status_t status = jpp_boot_append_event(context, JPP_BOOT_EVENT_STORAGE_READY);
    if (status == JPP_BOOT_STATUS_OK) {
        context->flash_layout_ready = true;
    }
    return status;
}

jpp_boot_status_t jpp_boot_note_settings_ready(
    jpp_boot_context_t *context,
    const jpp_settings_load_result_t *settings_state
)
{
    jpp_boot_status_t status;
    if (context == NULL || settings_state == NULL) {
        return JPP_BOOT_STATUS_INVALID_ARGUMENT;
    }
    status = jpp_boot_append_event(context, JPP_BOOT_EVENT_SETTINGS_READY);
    if (status == JPP_BOOT_STATUS_OK) {
        context->settings_available = true;
        context->settings_state = *settings_state;
    }
    return status;
}

jpp_boot_status_t jpp_boot_note_sd_mount(
    jpp_boot_context_t *context,
    bool mounted,
    bool forced_recovery,
    const char *sd_mount_reason
)
{
    jpp_boot_status_t status = jpp_boot_append_event(context, JPP_BOOT_EVENT_SD_MOUNT_READY);
    if (status != JPP_BOOT_STATUS_OK) {
        return status;
    }
    context->sd_mount_attempted = true;
    context->sd_mounted = mounted;
    context->sd_mount_reason = sd_mount_reason != NULL ? sd_mount_reason : "unknown";
    if (mounted && !forced_recovery) {
        context->boot_mode = JPP_BOOT_MODE_NORMAL;
        context->boot_reason = context->sd_mount_reason;
        return JPP_BOOT_STATUS_OK;
    }
    context->boot_mode = JPP_BOOT_MODE_RECOVERY;
    context->boot_reason = forced_recovery ? "forced_recovery" : context->sd_mount_reason;
    return JPP_BOOT_STATUS_OK;
}

jpp_boot_status_t jpp_boot_note_runtime_paths_ready(jpp_boot_context_t *context)
{
    jpp_boot_status_t status = jpp_boot_append_event(context, JPP_BOOT_EVENT_RUNTIME_PATHS_READY);
    if (status == JPP_BOOT_STATUS_OK) {
        context->runtime_paths_ready = true;
    }
    return status;
}

jpp_boot_status_t jpp_boot_note_services_ready(jpp_boot_context_t *context)
{
    jpp_boot_status_t status = jpp_boot_append_event(context, JPP_BOOT_EVENT_SERVICES_READY);
    if (status == JPP_BOOT_STATUS_OK) {
        context->services_ready = true;
    }
    return status;
}

jpp_boot_status_t jpp_boot_note_discovery_ready(
    jpp_boot_context_t *context,
    const jpp_boot_discovery_summary_t *summary
)
{
    jpp_boot_status_t status;
    if (context == NULL || summary == NULL) {
        return JPP_BOOT_STATUS_INVALID_ARGUMENT;
    }
    status = jpp_boot_append_event(context, JPP_BOOT_EVENT_DISCOVERY_READY);
    if (status == JPP_BOOT_STATUS_OK) {
        context->discovery = *summary;
        context->discovery_ready = true;
    }
    return status;
}

jpp_boot_status_t jpp_boot_note_system_ready(jpp_boot_context_t *context)
{
    jpp_boot_status_t status = jpp_boot_append_event(context, JPP_BOOT_EVENT_SYSTEM_READY);
    if (status == JPP_BOOT_STATUS_OK) {
        context->system_ready = true;
    }
    return status;
}

const char *jpp_boot_event_name(jpp_boot_event_t event)
{
    switch (event) {
    case JPP_BOOT_EVENT_BOOT_START:
        return "BOOT_START";
    case JPP_BOOT_EVENT_STORAGE_READY:
        return "STORAGE_READY";
    case JPP_BOOT_EVENT_SETTINGS_READY:
        return "SETTINGS_READY";
    case JPP_BOOT_EVENT_SD_MOUNT_READY:
        return "SD_MOUNT_READY";
    case JPP_BOOT_EVENT_RUNTIME_PATHS_READY:
        return "RUNTIME_PATHS_READY";
    case JPP_BOOT_EVENT_SERVICES_READY:
        return "SERVICES_READY";
    case JPP_BOOT_EVENT_DISCOVERY_READY:
        return "DISCOVERY_READY";
    case JPP_BOOT_EVENT_SYSTEM_READY:
        return "SYSTEM_READY";
    }
    return "UNKNOWN";
}

const char *jpp_boot_mode_name(jpp_boot_mode_t mode)
{
    switch (mode) {
    case JPP_BOOT_MODE_NORMAL:
        return "normal";
    case JPP_BOOT_MODE_RECOVERY:
        return "recovery";
    case JPP_BOOT_MODE_UNKNOWN:
    default:
        return "unknown";
    }
}

const char *jpp_boot_status_name(jpp_boot_status_t status)
{
    switch (status) {
    case JPP_BOOT_STATUS_OK:
        return "OK";
    case JPP_BOOT_STATUS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case JPP_BOOT_STATUS_OUT_OF_ORDER:
        return "OUT_OF_ORDER";
    case JPP_BOOT_STATUS_OVERFLOW:
        return "OVERFLOW";
    case JPP_BOOT_STATUS_FINALIZED:
        return "FINALIZED";
    }
    return "UNKNOWN";
}
