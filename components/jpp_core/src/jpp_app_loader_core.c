#include "../include/jpp_app_loader_core.h"
#include "../include/jpp_string_util.h"

#include <string.h>

static jpp_app_loader_check_t make_check(
    jpp_app_loader_result_t result,
    jpp_manifest_result_t manifest_result,
    const jpp_manifest_v2_t *manifest,
    const jpp_app_entry_record_t *entry_record)
{
    jpp_app_loader_check_t check;

    check.result = result;
    check.manifest_result = manifest_result;
    check.app_id = manifest != NULL ? manifest->app_id : NULL;
    if (entry_record != NULL && jpp_str_nonempty(entry_record->path)) {
        check.entry_path = entry_record->path;
    } else {
        check.entry_path = manifest != NULL ? manifest->entry : NULL;
    }
    return check;
}

static int entry_toolchain_is_complete(const jpp_app_entry_record_t *entry_record)
{
    return jpp_str_nonempty(entry_record->runtime_version) &&
           jpp_str_nonempty(entry_record->cross_version) &&
           entry_record->bytecode_abi > 0;
}

static int entry_toolchain_matches(const jpp_app_entry_record_t *entry_record)
{
    return jpp_str_eq(entry_record->runtime_version, JPP_MANIFEST_V2_RUNTIME_VERSION) &&
           jpp_str_eq(entry_record->cross_version, JPP_MANIFEST_V2_CROSS_VERSION) &&
           entry_record->bytecode_abi == JPP_MANIFEST_V2_BYTECODE_ABI;
}

jpp_app_loader_check_t jpp_app_loader_preflight(
    const jpp_manifest_v2_t *manifest,
    const jpp_app_entry_record_t *entry_record)
{
    jpp_manifest_result_t manifest_result;

    manifest_result = jpp_manifest_v2_validate(manifest);
    if (manifest_result != JPP_MANIFEST_OK) {
        return make_check(
            JPP_APP_LOADER_MANIFEST_REJECTED,
            manifest_result,
            manifest,
            entry_record);
    }
    if (entry_record == NULL || !entry_record->present) {
        return make_check(JPP_APP_LOADER_MISSING_ENTRY, manifest_result, manifest, entry_record);
    }
    if (!jpp_str_eq(entry_record->path, manifest->entry) ||
        !jpp_manifest_v2_is_valid_entry_path(entry_record->path)) {
        return make_check(JPP_APP_LOADER_INVALID_ENTRY, manifest_result, manifest, entry_record);
    }
    if (entry_record->size_bytes == 0) {
        return make_check(JPP_APP_LOADER_CORRUPT, manifest_result, manifest, entry_record);
    }
    if (manifest->app_type == JPP_APP_TYPE_MICROPYTHON) {
        if (!entry_toolchain_is_complete(entry_record)) {
            return make_check(JPP_APP_LOADER_INVALID_TOOLCHAIN, manifest_result, manifest, entry_record);
        }
        if (!entry_toolchain_matches(entry_record)) {
            return make_check(JPP_APP_LOADER_RUNTIME_MISMATCH, manifest_result, manifest, entry_record);
        }
    }
    return make_check(JPP_APP_LOADER_OK, manifest_result, manifest, entry_record);
}

const char *jpp_app_loader_result_name(jpp_app_loader_result_t result)
{
    switch (result) {
    case JPP_APP_LOADER_OK:
        return "OK";
    case JPP_APP_LOADER_MANIFEST_REJECTED:
        return "MANIFEST_REJECTED";
    case JPP_APP_LOADER_MISSING_ENTRY:
        return "MISSING_ENTRY";
    case JPP_APP_LOADER_INVALID_ENTRY:
        return "INVALID_ENTRY";
    case JPP_APP_LOADER_INVALID_TOOLCHAIN:
        return "INVALID_TOOLCHAIN";
    case JPP_APP_LOADER_RUNTIME_MISMATCH:
        return "RUNTIME_MISMATCH";
    case JPP_APP_LOADER_CORRUPT:
        return "CORRUPT";
    default:
        return "UNKNOWN";
    }
}
