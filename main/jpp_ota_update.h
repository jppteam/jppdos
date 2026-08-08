#pragma once

/*
 * Firmware self-update: check GitHub Releases, stage the app-only image on
 * SD, verify it, then erase+rewrite the running `factory` partition.
 *
 * This device has one app partition — no ota_0/ota_1 spare slot, no otadata
 * — so this is what ESP-IDF's own docs call "Unsafe Update Mode": there is
 * no fallback image if power is lost mid-write, and esp_ota_ops (esp_ota_
 * begin/write/end) cannot even be used here, since esp_ota_begin() refuses
 * both a non-OTA-subtype partition and the currently-running partition
 * (ESP_ERR_OTA_PARTITION_CONFLICT). This module instead drives the raw
 * esp_partition_erase_range()/_write()/_read() API directly and substitutes
 * its own integrity check — a SHA-256 verify of the SD-staged file against
 * the release's published SHA256SUMS.txt, then a full readback-compare of
 * flash against that same verified file — for the checks esp_ota_end()
 * would otherwise have done. See AGENTS.md "Firmware update (OTA)".
 *
 * Every function below except the two build-identity accessors is blocking
 * (network and/or flash I/O) and meant to be called the same way the
 * existing settings screen calls do_wifi_connect/do_settings_restore: from
 * within jpp_settings_screen's render/handle callbacks, on the main task.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Build identity (baked in at compile time — see main/CMakeLists.txt,
   "Build identity" comment, and jpp_build_info.h which it generates) ------- */

/* Unix epoch of this build's tagged commit; 0 for a local/dev build (so any
   real release always compares as newer). */
uint32_t jpp_ota_build_timestamp(void);
/* Short commit SHA this build was cut from, or "dev". Display-only. */
const char *jpp_ota_build_commit(void);
/* True if this build was itself cut from the pre-release channel (or is a
   local/dev build) — used to pre-enable the "Update to pre-releases" toggle
   the first time Settings loads it. */
bool jpp_ota_build_is_prerelease(void);

/* ---- Release check --------------------------------------------------------- */

#define JPP_OTA_TAG_MAX        32u
#define JPP_OTA_ASSET_NAME_MAX 64u
#define JPP_OTA_URL_MAX        192u

typedef struct {
    bool     available;        /* candidate's published_at is newer than this build */
    char     tag[JPP_OTA_TAG_MAX];
    bool     is_prerelease;
    uint32_t published_at_epoch;
    char     asset_name[JPP_OTA_ASSET_NAME_MAX]; /* e.g. "jppdos-1.3-esp32c6-app.bin" */
    char     asset_url[JPP_OTA_URL_MAX];         /* browser_download_url of the app-only .bin */
    uint32_t asset_size;                         /* bytes, from the release API's asset "size" */
    char     sha_url[JPP_OTA_URL_MAX];           /* browser_download_url of SHA256SUMS.txt */
} jpp_ota_release_info_t;

typedef enum {
    JPP_OTA_CHECK_OK = 0,
    JPP_OTA_CHECK_ERR_WIFI,       /* not connected */
    JPP_OTA_CHECK_ERR_HTTP,       /* request/transport failed */
    JPP_OTA_CHECK_ERR_PARSE,      /* unexpected/oversized JSON */
    JPP_OTA_CHECK_ERR_NO_ASSET,   /* release has no matching app.bin + SHA256SUMS.txt pair */
} jpp_ota_check_status_t;

/*
 * Blocking. Requires Wi-Fi already connected.
 *
 * include_prereleases picks the GitHub endpoint:
 *   false -> GET /repos/<owner>/<repo>/releases/latest
 *            (GitHub's own "latest" — excludes pre-releases and drafts)
 *   true  -> GET /repos/<owner>/<repo>/releases?per_page=1
 *            (the single most-recently-created release of either channel,
 *             which is what "opted into pre-releases" should mean: newest
 *             of everything, not newest-of-a-filtered-list)
 * Both return one release object's worth of JSON, kept small on purpose —
 * unlike the full list endpoint, this never needs client-side filtering.
 */
jpp_ota_check_status_t jpp_ota_check_for_update(bool include_prereleases,
                                                 jpp_ota_release_info_t *out);

/* ---- Download + verify (fully reversible — nothing here touches flash) ---- */

#define JPP_OTA_STAGED_PATH_MAX 96u

typedef enum {
    JPP_OTA_DL_OK = 0,
    JPP_OTA_DL_ERR_HTTP,
    JPP_OTA_DL_ERR_SD,
    JPP_OTA_DL_ERR_HASH_FETCH,
    JPP_OTA_DL_ERR_HASH_MISMATCH,
} jpp_ota_download_status_t;

typedef void (*jpp_ota_progress_cb)(size_t bytes_done, size_t bytes_total, void *ctx);

/* Which blocking phase a jpp_ota_progress_cb is reporting for — lets the UI
   layer (jpp_settings_screen_render_ota_progress()) switch between the
   "Downloading" screen and the "Do NOT power off" installing screen without
   the download/flash code here knowing anything about the display. */
typedef enum {
    JPP_OTA_PROGRESS_DOWNLOADING = 0,
    JPP_OTA_PROGRESS_INSTALLING,
} jpp_ota_progress_phase_t;

/*
 * Blocking. Downloads info->asset_url to /sd/updates/<asset_name>, then
 * fetches info->sha_url and checks the staged file's SHA-256 against the
 * line matching info->asset_name. On any non-OK return the staged file (if
 * one was written) is deleted — jpp_ota_flash_and_reboot() must never be
 * handed a file this function did not itself just verify.
 */
jpp_ota_download_status_t jpp_ota_download_update(
    const jpp_ota_release_info_t *info,
    jpp_ota_progress_cb progress_cb, void *progress_ctx,
    char *out_path, size_t out_path_len);

/* ---- Pre-flight (still non-destructive) ------------------------------------ */

#define JPP_OTA_MIN_BATTERY_PCT 30

typedef enum {
    JPP_OTA_PREFLIGHT_OK = 0,
    JPP_OTA_PREFLIGHT_ERR_BAD_IMAGE,   /* magic byte check failed, or unreadable */
    JPP_OTA_PREFLIGHT_ERR_TOO_LARGE,   /* staged file bigger than the factory partition */
    JPP_OTA_PREFLIGHT_ERR_BATTERY_LOW, /* below JPP_OTA_MIN_BATTERY_PCT */
} jpp_ota_preflight_status_t;

/* battery_valid=false (no battery sensor / bench unit) skips the battery
   gate rather than blocking on an unreadable reading. */
jpp_ota_preflight_status_t jpp_ota_preflight_check(const char *staged_path,
                                                    int battery_pct,
                                                    bool battery_valid);

/* ---- Flash + reboot (point of no return) ----------------------------------- */

typedef enum {
    JPP_OTA_FLASH_ERR_OPEN = 0,     /* could not open/stat the staged file */
    JPP_OTA_FLASH_ERR_NO_PARTITION, /* factory partition not found, or file too big for it */
    JPP_OTA_FLASH_ERR_ERASE,
    JPP_OTA_FLASH_ERR_WRITE,
    JPP_OTA_FLASH_ERR_VERIFY,       /* readback did not match the staged file */
} jpp_ota_flash_error_t;

/*
 * Disconnects Wi-Fi, then erases and rewrites the running `factory`
 * partition from staged_path, verifies by reading it back and comparing
 * against the staged file, and reboots (esp_restart) on success — this call
 * does not return on success.
 *
 * The caller must already have shown a "do not power off" screen before
 * calling this: once JPP_OTA_FLASH_ERR_ERASE or later is returned, flash no
 * longer reliably holds the image that was running before this call, and
 * there is no fallback partition to boot instead (see AGENTS.md "Firmware
 * update (OTA)"). JPP_OTA_FLASH_ERR_OPEN/_NO_PARTITION are returned before
 * anything destructive happens.
 */
jpp_ota_flash_error_t jpp_ota_flash_and_reboot(const char *staged_path,
                                                jpp_ota_progress_cb progress_cb,
                                                void *progress_ctx);

/* ---- Display strings -------------------------------------------------------- */

const char *jpp_ota_check_status_str(jpp_ota_check_status_t s);
const char *jpp_ota_download_status_str(jpp_ota_download_status_t s);
const char *jpp_ota_preflight_status_str(jpp_ota_preflight_status_t s);
const char *jpp_ota_flash_error_str(jpp_ota_flash_error_t s);

#ifdef __cplusplus
}
#endif
