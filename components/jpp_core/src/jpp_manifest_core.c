#include "../include/jpp_manifest_core.h"
#include "../include/jpp_resource_budget.h"
#include "../include/jpp_string_util.h"

#include <string.h>

static int has_suffix(const char *str, const char *suffix)
{
    size_t str_len = strlen(str);
    size_t suf_len = strlen(suffix);
    return str_len >= suf_len && strcmp(str + str_len - suf_len, suffix) == 0;
}

int jpp_manifest_v2_is_reserved_app_id(const char *app_id)
{
    /* Must match every screen name the launcher/main loop routes by id. */
    static const char *const reserved[] = {
        "launcher", "settings", "webdav", "webdav_passconfig",
        "shell", "dialog", "app_crash", "sd_ejected",
    };
    for (size_t i = 0u; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (jpp_str_eq(app_id, reserved[i])) {
            return 1;
        }
    }
    return 0;
}

int jpp_manifest_v2_is_allowed_capability(const char *capability)
{
    /*
     * Only capabilities that require a user prompt are declarable.  Everything
     * else (scoped/shared storage, device status, IPC, the key-value helper,
     * frame/canvas/keys/buzzer/wakelock) is available to every app without
     * declaration or tracking — the firmware enforces scoping, not permission.
     */
    /* Tier 1 — one-time user grant, persisted */
    if (jpp_str_eq(capability, "http.request") ||
        jpp_str_eq(capability, "ble.scan") ||
        jpp_str_eq(capability, "ble.advertise") ||
        jpp_str_eq(capability, "background.register") ||
        jpp_str_eq(capability, "esp_now")) {
        return 1;
    }
    /* Tier 2 — per-session user grant */
    if (jpp_str_eq(capability, "files.full") ||
        jpp_str_eq(capability, "network.bind") ||
        jpp_str_eq(capability, "ble.connect") ||
        jpp_str_eq(capability, "ble.host")) {
        return 1;
    }
    return 0;
}

int jpp_manifest_v2_is_valid_entry_path(const char *path)
{
    if (!jpp_str_nonempty(path)) {
        return 0;
    }
    if (path[0] == '/') {
        return 0;
    }
    if (jpp_str_has_parent_segment(path)) {
        return 0;
    }
    return 1;
}

static jpp_manifest_result_t validate_capabilities(const jpp_manifest_v2_t *manifest)
{
    size_t index;

    if (manifest->capability_count > 0 && manifest->capabilities == NULL) {
        return JPP_MANIFEST_INVALID_MANIFEST;
    }
    for (index = 0; index < manifest->capability_count; index++) {
        if (!jpp_manifest_v2_is_allowed_capability(manifest->capabilities[index])) {
            return JPP_MANIFEST_INVALID_CAPABILITY;
        }
    }
    return JPP_MANIFEST_OK;
}

/* One cron field: "*" or a single number within [min_v, max_v]. Advances
   *cursor past the field and any trailing spaces; returns 0 on bad input. */
static int cron_field_ok(const char **cursor, int min_v, int max_v)
{
    const char *p = *cursor;
    if (*p == '*') {
        p++;
    } else {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        int v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            p++;
        }
        if (v < min_v || v > max_v) {
            return 0;
        }
    }
    if (*p != ' ' && *p != '\0') {
        return 0;
    }
    while (*p == ' ') {
        p++;
    }
    *cursor = p;
    return 1;
}

int jpp_manifest_v2_is_valid_cron(const char *cron)
{
    if (!jpp_str_nonempty(cron)) {
        return 0;
    }
    const char *p = cron;
    if (!cron_field_ok(&p, 0, 59)) { return 0; }   /* minute */
    if (!cron_field_ok(&p, 0, 23)) { return 0; }   /* hour */
    if (!cron_field_ok(&p, 1, 31)) { return 0; }   /* day of month */
    if (!cron_field_ok(&p, 1, 12)) { return 0; }   /* month */
    if (!cron_field_ok(&p, 0, 6))  { return 0; }   /* day of week (0 = Sunday) */
    return *p == '\0';
}

static jpp_manifest_result_t validate_background(const jpp_manifest_v2_t *manifest)
{
    if (manifest->background.enabled != 0 && manifest->background.enabled != 1) {
        return JPP_MANIFEST_INVALID_BACKGROUND;
    }
    if (manifest->background.task_count > JPP_MANIFEST_V2_BG_TASK_MAX) {
        return JPP_MANIFEST_INVALID_BACKGROUND;
    }
    if (manifest->background.task_count > 0 && manifest->background.tasks == NULL) {
        return JPP_MANIFEST_INVALID_BACKGROUND;
    }
    for (size_t i = 0; i < manifest->background.task_count; i++) {
        const jpp_manifest_bg_task_t *task = &manifest->background.tasks[i];
        if (!jpp_str_name_valid(task->name)) {
            return JPP_MANIFEST_INVALID_BACKGROUND;
        }
        int has_interval = task->interval_s > 0u;
        int has_cron     = jpp_str_nonempty(task->cron);
        if (has_interval == has_cron) {   /* exactly one source */
            return JPP_MANIFEST_INVALID_BACKGROUND;
        }
        if (has_interval && task->interval_s < JPP_RESOURCE_BG_INTERVAL_MIN_S) {
            return JPP_MANIFEST_INVALID_BACKGROUND;
        }
        if (has_cron && !jpp_manifest_v2_is_valid_cron(task->cron)) {
            return JPP_MANIFEST_INVALID_BACKGROUND;
        }
    }
    return JPP_MANIFEST_OK;
}

static jpp_manifest_result_t validate_toolchain(const jpp_manifest_v2_t *manifest)
{
    if (manifest->app_type == JPP_APP_TYPE_NATIVE) {
        return JPP_MANIFEST_OK;
    }
    if (!jpp_str_nonempty(manifest->toolchain.runtime_version) ||
        !jpp_str_nonempty(manifest->toolchain.cross_version) ||
        manifest->toolchain.bytecode_abi <= 0) {
        return JPP_MANIFEST_INVALID_TOOLCHAIN;
    }
    if (!jpp_str_eq(manifest->toolchain.runtime_version, JPP_MANIFEST_V2_RUNTIME_VERSION) ||
        !jpp_str_eq(manifest->toolchain.cross_version, JPP_MANIFEST_V2_CROSS_VERSION) ||
        manifest->toolchain.bytecode_abi != JPP_MANIFEST_V2_BYTECODE_ABI) {
        return JPP_MANIFEST_RUNTIME_MISMATCH;
    }
    return JPP_MANIFEST_OK;
}

static jpp_manifest_result_t validate_app_type_entry(const jpp_manifest_v2_t *manifest)
{
    if (manifest->app_type == JPP_APP_TYPE_MICROPYTHON) {
        if (!has_suffix(manifest->entry, ".mpy")) {
            return JPP_MANIFEST_INVALID_APP_TYPE;
        }
    } else if (manifest->app_type == JPP_APP_TYPE_NATIVE) {
        if (!has_suffix(manifest->entry, ".bin")) {
            return JPP_MANIFEST_INVALID_APP_TYPE;
        }
    } else {
        return JPP_MANIFEST_INVALID_APP_TYPE;
    }
    return JPP_MANIFEST_OK;
}

jpp_manifest_result_t jpp_manifest_v2_validate(const jpp_manifest_v2_t *manifest)
{
    jpp_manifest_result_t result;

    if (manifest == NULL) {
        return JPP_MANIFEST_INVALID_MANIFEST;
    }
    if (manifest->schema_version != JPP_MANIFEST_V2_SCHEMA_VERSION) {
        return JPP_MANIFEST_SCHEMA_MISMATCH;
    }
    if (!jpp_str_nonempty(manifest->app_id) || !jpp_str_nonempty(manifest->name) ||
        !jpp_str_nonempty(manifest->version) || !jpp_str_nonempty(manifest->entry)) {
        return JPP_MANIFEST_INVALID_MANIFEST;
    }
    if (jpp_manifest_v2_is_reserved_app_id(manifest->app_id)) {
        return JPP_MANIFEST_RESERVED_APP_ID;
    }
    if (manifest->sdk_min > manifest->sdk_max) {
        return JPP_MANIFEST_INVALID_MANIFEST;
    }
    if (!jpp_manifest_v2_is_valid_entry_path(manifest->entry)) {
        return JPP_MANIFEST_INVALID_ENTRY;
    }
    result = validate_app_type_entry(manifest);
    if (result != JPP_MANIFEST_OK) {
        return result;
    }
    result = validate_capabilities(manifest);
    if (result != JPP_MANIFEST_OK) {
        return result;
    }
    result = validate_background(manifest);
    if (result != JPP_MANIFEST_OK) {
        return result;
    }
    return validate_toolchain(manifest);
}

const char *jpp_manifest_result_name(jpp_manifest_result_t result)
{
    switch (result) {
    case JPP_MANIFEST_OK:
        return "OK";
    case JPP_MANIFEST_INVALID_MANIFEST:
        return "INVALID_MANIFEST";
    case JPP_MANIFEST_SCHEMA_MISMATCH:
        return "SCHEMA_MISMATCH";
    case JPP_MANIFEST_RESERVED_APP_ID:
        return "RESERVED_APP_ID";
    case JPP_MANIFEST_INVALID_ENTRY:
        return "INVALID_ENTRY";
    case JPP_MANIFEST_INVALID_APP_TYPE:
        return "INVALID_APP_TYPE";
    case JPP_MANIFEST_INVALID_CAPABILITY:
        return "INVALID_CAPABILITY";
    case JPP_MANIFEST_INVALID_BACKGROUND:
        return "INVALID_BACKGROUND";
    case JPP_MANIFEST_INVALID_TOOLCHAIN:
        return "INVALID_TOOLCHAIN";
    case JPP_MANIFEST_RUNTIME_MISMATCH:
        return "RUNTIME_MISMATCH";
    default:
        return "UNKNOWN";
    }
}
