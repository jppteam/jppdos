#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_MANIFEST_V2_SCHEMA_VERSION 2
#define JPP_MANIFEST_V2_RUNTIME_VERSION "v1.28.0"
#define JPP_MANIFEST_V2_CROSS_VERSION "1.28.0"
#define JPP_MANIFEST_V2_BYTECODE_ABI 6

typedef enum {
    JPP_APP_TYPE_MICROPYTHON = 0,
    JPP_APP_TYPE_NATIVE,
} jpp_manifest_app_type_t;

typedef enum {
    JPP_MANIFEST_OK = 0,
    JPP_MANIFEST_INVALID_MANIFEST,
    JPP_MANIFEST_SCHEMA_MISMATCH,
    JPP_MANIFEST_RESERVED_APP_ID,
    JPP_MANIFEST_INVALID_ENTRY,
    JPP_MANIFEST_INVALID_APP_TYPE,
    JPP_MANIFEST_INVALID_CAPABILITY,
    JPP_MANIFEST_INVALID_BACKGROUND,
    JPP_MANIFEST_INVALID_TOOLCHAIN,
    JPP_MANIFEST_RUNTIME_MISMATCH,
} jpp_manifest_result_t;

typedef struct {
    int enabled;
    const char *mode;
} jpp_manifest_background_t;

typedef struct {
    const char *runtime_version;
    const char *cross_version;
    int bytecode_abi;
} jpp_manifest_toolchain_t;

typedef struct {
    int schema_version;
    const char *app_id;
    const char *name;
    const char *version;
    int sdk_min;
    int sdk_max;
    const char *entry;
    jpp_manifest_app_type_t app_type;
    const char *const *capabilities;
    size_t capability_count;
    jpp_manifest_background_t background;
    jpp_manifest_toolchain_t toolchain;
} jpp_manifest_v2_t;

jpp_manifest_result_t jpp_manifest_v2_validate(const jpp_manifest_v2_t *manifest);
const char *jpp_manifest_result_name(jpp_manifest_result_t result);
int jpp_manifest_v2_is_allowed_capability(const char *capability);
int jpp_manifest_v2_is_valid_entry_path(const char *path);

#ifdef __cplusplus
}
#endif
