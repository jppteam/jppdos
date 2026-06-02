#include "../include/jpp_manifest_core.h"
#include "../include/jpp_string_util.h"

#include <string.h>

static int has_suffix(const char *str, const char *suffix)
{
    size_t str_len = strlen(str);
    size_t suf_len = strlen(suffix);
    return str_len >= suf_len && strcmp(str + str_len - suf_len, suffix) == 0;
}

static int has_parent_segment(const char *path)
{
    const char *cursor;

    if (jpp_str_eq(path, "..")) {
        return 1;
    }
    cursor = path;
    while (cursor != NULL && cursor[0] != '\0') {
        if (cursor[0] == '.' && cursor[1] == '.' &&
            (cursor[2] == '/' || cursor[2] == '\0')) {
            return 1;
        }
        cursor = strchr(cursor, '/');
        if (cursor == NULL) {
            return 0;
        }
        cursor++;
    }
    return 0;
}

int jpp_manifest_v2_is_allowed_capability(const char *capability)
{
    /* Tier 0 — auto-granted */
    if (jpp_str_eq(capability, "files.scoped") ||
        jpp_str_eq(capability, "files.shared") ||
        jpp_str_eq(capability, "device.status")) {
        return 1;
    }
    /* Tier 1 — one-time user grant */
    if (jpp_str_eq(capability, "ipc.send") ||
        jpp_str_eq(capability, "http.request") ||
        jpp_str_eq(capability, "device.kv") ||
        jpp_str_eq(capability, "ble.scan") ||
        jpp_str_eq(capability, "ble.advertise")) {
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
    if (has_parent_segment(path)) {
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

static jpp_manifest_result_t validate_background(const jpp_manifest_v2_t *manifest)
{
    if (manifest->background.enabled != 0 && manifest->background.enabled != 1) {
        return JPP_MANIFEST_INVALID_BACKGROUND;
    }
    if (!jpp_str_eq(manifest->background.mode, "serialized")) {
        return JPP_MANIFEST_INVALID_BACKGROUND;
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
    if (jpp_str_eq(manifest->app_id, "settings")) {
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
