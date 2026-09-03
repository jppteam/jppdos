#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_MANIFEST_V2_SCHEMA_VERSION 2
#define JPP_MANIFEST_V2_RUNTIME_VERSION "v1.28.0"
#define JPP_MANIFEST_V2_CROSS_VERSION "1.28.0"
#define JPP_MANIFEST_V2_BYTECODE_ABI 6

/*
 * Native SDK API level exported by this firmware.  An app declares the minimum
 * level it needs via manifest `sdk_min`; the loader rejects apps that require a
 * newer SDK than the running firmware (JPP_SDK_VERSION < sdk_min).  Bump this
 * whenever a backward-compatible symbol/capability is ADDED to the SDK surface.
 *
 *   1 — initial release                        (shipped in firmware v1.0-RTM)
 *   2 — adds network.connect (outbound TCP), the crypto primitives
 *       (sha256/sha1, aes256-ige, modexp/rsa_encrypt/dh_compute), and
 *       https.request (TLS-verified HTTP client, per-origin consent)
 *                                              (shipped in firmware v1.1)
 *   3 — makes jpp_sdk_wrap_text callable from a loaded app binary; it was
 *       declared and documented from v1.0-RTM but never listed in s_symtab,
 *       so every native app calling it died at launch with UNRESOLVED_SYM
 *                                              (UNRELEASED — still open)
 *
 * Levels 1 and 2 are RELEASED and therefore CLOSED: an app declaring sdk_min
 * for a closed level must mean exactly one surface in the field, so nothing may
 * be folded into them.  Level 3 is open until a firmware release ships it, and
 * further additions made before that release belong in 3 rather than minting 4
 * — the same way level 2 accumulated four separate additions while unreleased.
 *
 * A branch that grows the SDK surface sets this to (last released level + 1),
 * NOT (master + 1): concurrent branches deliberately pick the same number and
 * merge cleanly.  If a release is cut while your branch is open, you must
 * re-target the level — git will NOT flag it, because both sides agree on the
 * value.  See the SDK versioning policy in AGENTS.md.
 */
#define JPP_SDK_VERSION 3

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
    JPP_MANIFEST_SDK_TOO_OLD,
} jpp_manifest_result_t;

#define JPP_MANIFEST_V2_BG_TASK_MAX 4u

/* One scheduled background task: exactly one of interval_s (seconds, at least
   JPP_RESOURCE_BG_INTERVAL_MIN_S) or cron (5-field "min hour dom mon dow",
   each field "*" or a single number) must be set. */
typedef struct {
    const char *name;
    unsigned    interval_s;   /* 0 = cron-scheduled */
    const char *cron;         /* NULL or "" when interval-based */
} jpp_manifest_bg_task_t;

typedef struct {
    int enabled;
    const jpp_manifest_bg_task_t *tasks;
    size_t task_count;
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

/*
 * True for app ids that collide with built-in screen names (launcher,
 * settings, webdav, dialogs, ...).  The launcher routes screens by name, so an
 * SD app with one of these ids could never be opened; manifests using them are
 * rejected and discovery skips matching directories.
 */
int jpp_manifest_v2_is_reserved_app_id(const char *app_id);

/* True for a valid 5-field cron expression ("min hour dom mon dow", each field
   "*" or a single in-range number). */
int jpp_manifest_v2_is_valid_cron(const char *cron);

#ifdef __cplusplus
}
#endif
