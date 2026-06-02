#pragma once

#include "jpp_manifest_core.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPP_APP_LOADER_OK = 0,
    JPP_APP_LOADER_MANIFEST_REJECTED,
    JPP_APP_LOADER_MISSING_ENTRY,
    JPP_APP_LOADER_INVALID_ENTRY,
    JPP_APP_LOADER_INVALID_TOOLCHAIN,
    JPP_APP_LOADER_RUNTIME_MISMATCH,
    JPP_APP_LOADER_CORRUPT,
} jpp_app_loader_result_t;

typedef struct {
    const char *path;
    int present;
    size_t size_bytes;
    const char *runtime_version;
    const char *cross_version;
    int bytecode_abi;
} jpp_app_entry_record_t;

typedef struct {
    jpp_app_loader_result_t result;
    jpp_manifest_result_t manifest_result;
    const char *app_id;
    const char *entry_path;
} jpp_app_loader_check_t;

jpp_app_loader_check_t jpp_app_loader_preflight(
    const jpp_manifest_v2_t *manifest,
    const jpp_app_entry_record_t *entry_record);
const char *jpp_app_loader_result_name(jpp_app_loader_result_t result);

#ifdef __cplusplus
}
#endif
