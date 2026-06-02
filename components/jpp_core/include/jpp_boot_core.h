#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "jpp_settings_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_BOOT_EVENT_CAPACITY 8u

typedef enum {
    JPP_BOOT_MODE_UNKNOWN = 0,
    JPP_BOOT_MODE_NORMAL,
    JPP_BOOT_MODE_RECOVERY,
} jpp_boot_mode_t;

typedef enum {
    JPP_BOOT_EVENT_BOOT_START = 0,
    JPP_BOOT_EVENT_STORAGE_READY,
    JPP_BOOT_EVENT_SETTINGS_READY,
    JPP_BOOT_EVENT_SD_MOUNT_READY,
    JPP_BOOT_EVENT_RUNTIME_PATHS_READY,
    JPP_BOOT_EVENT_SERVICES_READY,
    JPP_BOOT_EVENT_DISCOVERY_READY,
    JPP_BOOT_EVENT_SYSTEM_READY,
} jpp_boot_event_t;

typedef enum {
    JPP_BOOT_STATUS_OK = 0,
    JPP_BOOT_STATUS_INVALID_ARGUMENT,
    JPP_BOOT_STATUS_OUT_OF_ORDER,
    JPP_BOOT_STATUS_OVERFLOW,
    JPP_BOOT_STATUS_FINALIZED,
} jpp_boot_status_t;

typedef struct {
    size_t builtin_count;
    size_t disabled_count;
    size_t rejected_count;
    size_t sd_count;
    size_t total_count;
} jpp_boot_discovery_summary_t;

typedef struct {
    jpp_boot_mode_t boot_mode;
    const char *boot_reason;
    const char *sd_mount_reason;
    bool flash_layout_ready;
    bool settings_available;
    jpp_settings_load_result_t settings_state;
    bool sd_mount_attempted;
    bool sd_mounted;
    bool runtime_paths_ready;
    bool services_ready;
    bool discovery_ready;
    bool system_ready;
    jpp_boot_discovery_summary_t discovery;
    size_t event_count;
    jpp_boot_event_t events[JPP_BOOT_EVENT_CAPACITY];
} jpp_boot_context_t;

void jpp_boot_context_init(jpp_boot_context_t *context);

jpp_boot_status_t jpp_boot_note_storage_ready(jpp_boot_context_t *context);
jpp_boot_status_t jpp_boot_note_settings_ready(
    jpp_boot_context_t *context,
    const jpp_settings_load_result_t *settings_state
);
jpp_boot_status_t jpp_boot_note_sd_mount(
    jpp_boot_context_t *context,
    bool mounted,
    bool forced_recovery,
    const char *sd_mount_reason
);
jpp_boot_status_t jpp_boot_note_runtime_paths_ready(jpp_boot_context_t *context);
jpp_boot_status_t jpp_boot_note_services_ready(jpp_boot_context_t *context);
jpp_boot_status_t jpp_boot_note_discovery_ready(
    jpp_boot_context_t *context,
    const jpp_boot_discovery_summary_t *summary
);
jpp_boot_status_t jpp_boot_note_system_ready(jpp_boot_context_t *context);

const char *jpp_boot_event_name(jpp_boot_event_t event);
const char *jpp_boot_mode_name(jpp_boot_mode_t mode);
const char *jpp_boot_status_name(jpp_boot_status_t status);

#ifdef __cplusplus
}
#endif
