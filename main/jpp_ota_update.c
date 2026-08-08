#include "jpp_ota_update.h"
#include "jpp_build_info.h"
#include "jpp_wifi_init.h"
#include "jpp_file_util.h"
#include "jpp_heap_monitor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

static const char *TAG = "ota_update";

#define JPP_OTA_GITHUB_OWNER "jppteam"
#define JPP_OTA_GITHUB_REPO  "jppdos"

/* Small JSON responses only (one release object) — the release list/latest
   endpoints, unlike the full list, never need this to hold more than one
   release's worth of fields. A release with unusually long notes can still
   exceed this; that surfaces as JPP_OTA_CHECK_ERR_PARSE, not a truncation
   bug, since the body is NUL-terminated at whatever was captured. */
#define JPP_OTA_HTTP_BUF_BYTES 8192u

/* One buffer size for SD read, partition write, and partition read-back. */
#define JPP_OTA_CHUNK_BYTES 4096u

#define JPP_OTA_ERASE_ALIGN 4096u

/* GitHub release assets are always served via a 302 to a CDN URL.
   esp_http_client_perform() follows redirects internally, but the manual
   open/fetch_headers/read streaming API (needed here so a multi-MB image is
   never held in RAM at once) does not — that following only happens inside
   esp_http_client_perform()'s own state machine. Bounded so a redirect loop
   can't hang the download forever. */
#define JPP_OTA_MAX_REDIRECTS 5u

/* ---- Build identity --------------------------------------------------------- */

uint32_t jpp_ota_build_timestamp(void) { return (uint32_t)JPP_BUILD_TIMESTAMP; }
const char *jpp_ota_build_commit(void) { return JPP_BUILD_COMMIT; }
bool jpp_ota_build_is_prerelease(void) { return JPP_BUILD_PRERELEASE != 0; }

/* ---- Display strings ---------------------------------------------------------- */

const char *jpp_ota_check_status_str(jpp_ota_check_status_t s)
{
    switch (s) {
    case JPP_OTA_CHECK_OK:           return "OK";
    case JPP_OTA_CHECK_ERR_WIFI:     return "No Wi-Fi";
    case JPP_OTA_CHECK_ERR_HTTP:     return "Network error";
    case JPP_OTA_CHECK_ERR_PARSE:    return "Bad response";
    case JPP_OTA_CHECK_ERR_NO_ASSET: return "No image in release";
    default:                         return "Unknown error";
    }
}

const char *jpp_ota_download_status_str(jpp_ota_download_status_t s)
{
    switch (s) {
    case JPP_OTA_DL_OK:                return "OK";
    case JPP_OTA_DL_ERR_HTTP:          return "Download failed";
    case JPP_OTA_DL_ERR_SD:            return "SD card error";
    case JPP_OTA_DL_ERR_HASH_FETCH:    return "Checksum fetch failed";
    case JPP_OTA_DL_ERR_HASH_MISMATCH: return "Checksum mismatch";
    default:                           return "Unknown error";
    }
}

const char *jpp_ota_preflight_status_str(jpp_ota_preflight_status_t s)
{
    switch (s) {
    case JPP_OTA_PREFLIGHT_OK:             return "OK";
    case JPP_OTA_PREFLIGHT_ERR_BAD_IMAGE:   return "Bad image file";
    case JPP_OTA_PREFLIGHT_ERR_TOO_LARGE:   return "Image too large";
    case JPP_OTA_PREFLIGHT_ERR_BATTERY_LOW: return "Battery too low";
    default:                                return "Unknown error";
    }
}

const char *jpp_ota_flash_error_str(jpp_ota_flash_error_t s)
{
    switch (s) {
    case JPP_OTA_FLASH_ERR_OPEN:          return "Could not open image";
    case JPP_OTA_FLASH_ERR_NO_PARTITION:  return "Image too large";
    case JPP_OTA_FLASH_ERR_ERASE:         return "Flash erase failed";
    case JPP_OTA_FLASH_ERR_WRITE:         return "Flash write failed";
    case JPP_OTA_FLASH_ERR_VERIFY:        return "Verify failed";
    default:                              return "Unknown error";
    }
}

/* ---- ISO 8601 UTC -> Unix epoch --------------------------------------------- *
 *
 * GitHub's `published_at` is always "YYYY-MM-DDTHH:MM:SSZ". Parsed by hand
 * (days-from-civil, Howard Hinnant's well-known algorithm) instead of
 * timegm()/mktime() so this does not depend on libc timezone plumbing this
 * firmware otherwise never touches.
 */
static bool parse_iso8601_utc(const char *s, uint32_t *out_epoch)
{
    int y, mo, d, h, mi, se;
    if (s == NULL || sscanf(s, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &se) != 6) {
        return false;
    }
    int64_t yy  = y - (mo <= 2 ? 1 : 0);
    int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    int64_t yoe = yy - era * 400;                                     /* [0, 399] */
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;    /* [0, 365] */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;               /* [0, 146096] */
    int64_t days  = era * 146097 + doe - 719468;                      /* since 1970-01-01 */
    int64_t epoch = days * 86400 + h * 3600 + mi * 60 + se;
    if (epoch < 0) {
        return false;
    }
    *out_epoch = (uint32_t)epoch;
    return true;
}

/* ---- Small-body HTTPS GET (release JSON, SHA256SUMS.txt) -------------------- */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} jpp_ota_body_buf_t;

static esp_err_t ota_body_event_handler(esp_http_client_event_t *evt)
{
    jpp_ota_body_buf_t *b = (jpp_ota_body_buf_t *)evt->user_data;
    if (b == NULL || evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    size_t remaining = (b->cap > b->len + 1u) ? (b->cap - b->len - 1u) : 0u;
    size_t to_copy = ((size_t)evt->data_len < remaining) ? (size_t)evt->data_len : remaining;
    if (to_copy > 0u) {
        memcpy(b->buf + b->len, evt->data, to_copy);
        b->len += to_copy;
        b->buf[b->len] = '\0';
    }
    return ESP_OK;
}

/* Blocking GET of a small text/JSON body into out_buf (NUL-terminated,
   truncated if larger than out_cap). Returns true only on HTTP 200. */
static bool ota_http_get_body(const char *url, char *out_buf, size_t out_cap)
{
    out_buf[0] = '\0';
    jpp_ota_body_buf_t body = { .buf = out_buf, .len = 0u, .cap = out_cap };

    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = ota_body_event_handler,
        .user_data         = &body,
        .timeout_ms        = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .transport_type    = HTTP_TRANSPORT_OVER_SSL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return false;
    }
    /* GitHub's API rejects requests with no User-Agent. */
    esp_http_client_set_header(client, "User-Agent", "JPPDOS-OTA");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
    } else if (status != 200) {
        ESP_LOGW(TAG, "GET %s -> HTTP %d", url, status);
    }
    return (err == ESP_OK && status == 200);
}

/* ---- Release check ----------------------------------------------------------- */

static bool asset_name_matches_app_bin(const char *name)
{
    static const char suffix[] = "-esp32c6-app.bin";
    size_t name_len = strlen(name);
    size_t suf_len  = sizeof(suffix) - 1u;
    return name_len > suf_len &&
           strcmp(name + (name_len - suf_len), suffix) == 0;
}

static jpp_ota_check_status_t parse_release_json(const cJSON *rel,
                                                   jpp_ota_release_info_t *out)
{
    const cJSON *tag_j     = cJSON_GetObjectItemCaseSensitive(rel, "tag_name");
    const cJSON *pub_j     = cJSON_GetObjectItemCaseSensitive(rel, "published_at");
    const cJSON *pre_j     = cJSON_GetObjectItemCaseSensitive(rel, "prerelease");
    const cJSON *assets_j  = cJSON_GetObjectItemCaseSensitive(rel, "assets");

    if (!cJSON_IsString(pub_j) || !parse_iso8601_utc(pub_j->valuestring,
                                                       &out->published_at_epoch)) {
        return JPP_OTA_CHECK_ERR_PARSE;
    }
    if (cJSON_IsString(tag_j)) {
        strncpy(out->tag, tag_j->valuestring, sizeof(out->tag) - 1u);
        out->tag[sizeof(out->tag) - 1u] = '\0';
    }
    out->is_prerelease = cJSON_IsTrue(pre_j);

    if (!cJSON_IsArray(assets_j)) {
        return JPP_OTA_CHECK_ERR_NO_ASSET;
    }

    bool have_asset = false, have_sha = false;
    int count = cJSON_GetArraySize(assets_j);
    for (int i = 0; i < count; i++) {
        const cJSON *a = cJSON_GetArrayItem(assets_j, i);
        const cJSON *name_j = cJSON_GetObjectItemCaseSensitive(a, "name");
        const cJSON *url_j  = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
        const cJSON *size_j = cJSON_GetObjectItemCaseSensitive(a, "size");
        if (!cJSON_IsString(name_j) || !cJSON_IsString(url_j)) {
            continue;
        }
        const char *name = name_j->valuestring;
        if (asset_name_matches_app_bin(name)) {
            strncpy(out->asset_name, name, sizeof(out->asset_name) - 1u);
            out->asset_name[sizeof(out->asset_name) - 1u] = '\0';
            strncpy(out->asset_url, url_j->valuestring, sizeof(out->asset_url) - 1u);
            out->asset_url[sizeof(out->asset_url) - 1u] = '\0';
            if (cJSON_IsNumber(size_j)) {
                out->asset_size = (uint32_t)size_j->valuedouble;
            }
            have_asset = true;
        } else if (strcmp(name, "SHA256SUMS.txt") == 0) {
            strncpy(out->sha_url, url_j->valuestring, sizeof(out->sha_url) - 1u);
            out->sha_url[sizeof(out->sha_url) - 1u] = '\0';
            have_sha = true;
        }
    }

    if (!have_asset || !have_sha) {
        return JPP_OTA_CHECK_ERR_NO_ASSET;
    }

    out->available = out->published_at_epoch > jpp_ota_build_timestamp();
    return JPP_OTA_CHECK_OK;
}

jpp_ota_check_status_t jpp_ota_check_for_update(bool include_prereleases,
                                                 jpp_ota_release_info_t *out)
{
    if (out == NULL) {
        return JPP_OTA_CHECK_ERR_HTTP;
    }
    memset(out, 0, sizeof(*out));

    if (!wifi_is_connected()) {
        return JPP_OTA_CHECK_ERR_WIFI;
    }

    char url[160];
    if (include_prereleases) {
        snprintf(url, sizeof(url),
                 "https://api.github.com/repos/%s/%s/releases?per_page=1",
                 JPP_OTA_GITHUB_OWNER, JPP_OTA_GITHUB_REPO);
    } else {
        snprintf(url, sizeof(url),
                 "https://api.github.com/repos/%s/%s/releases/latest",
                 JPP_OTA_GITHUB_OWNER, JPP_OTA_GITHUB_REPO);
    }

    char *buf = malloc(JPP_OTA_HTTP_BUF_BYTES);
    if (buf == NULL) {
        return JPP_OTA_CHECK_ERR_HTTP;
    }

    jpp_heap_monitor_log("ota-check-start");
    bool ok = ota_http_get_body(url, buf, JPP_OTA_HTTP_BUF_BYTES);
    jpp_heap_monitor_log("ota-check-stop");
    if (!ok) {
        free(buf);
        return JPP_OTA_CHECK_ERR_HTTP;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return JPP_OTA_CHECK_ERR_PARSE;
    }

    const cJSON *rel = include_prereleases ? cJSON_GetArrayItem(root, 0) : root;
    jpp_ota_check_status_t status;
    if (!cJSON_IsObject(rel)) {
        status = JPP_OTA_CHECK_ERR_PARSE;
    } else {
        status = parse_release_json(rel, out);
    }
    cJSON_Delete(root);
    return status;
}

/* ---- SHA-256 helpers --------------------------------------------------------- */

/* Streams the file through mbedtls's incremental SHA-256 rather than
   jpp_crypto_sha256() (a one-shot, whole-buffer digest) — the staged app
   image can be several MB, too large to hold in RAM at once. */
static bool sha256_file(const char *path, uint8_t out[32])
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }
    uint8_t *buf = malloc(JPP_OTA_CHUNK_BYTES);
    if (buf == NULL) {
        fclose(fp);
        return false;
    }
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0 /* SHA-256, not SHA-224 */);
    size_t n;
    while ((n = fread(buf, 1u, JPP_OTA_CHUNK_BYTES, fp)) > 0u) {
        mbedtls_sha256_update(&ctx, buf, n);
    }
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
    free(buf);
    fclose(fp);
    return true;
}

/* SHA256SUMS.txt is `sha256sum` output: "<64 hex chars>  <filename>\n" per
   line. Finds the line for `filename` and decodes its digest into out[32]. */
static bool find_sha256_line(const char *text, const char *filename, uint8_t out[32])
{
    size_t fn_len = strlen(filename);
    const char *line = text;
    while (line != NULL && *line != '\0') {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        if (line_len > 64u + 1u + fn_len) {
            const char *name_part = line + 64u;
            while (*name_part == ' ' || *name_part == '*') {
                name_part++;
            }
            if (strncmp(name_part, filename, fn_len) == 0) {
                char trail = name_part[fn_len];
                if (trail == '\0' || trail == '\n' || trail == '\r') {
                    bool hex_ok = true;
                    for (size_t i = 0; i < 32u; i++) {
                        char byte_hex[3] = { line[i * 2u], line[i * 2u + 1u], '\0' };
                        char *end = NULL;
                        long v = strtol(byte_hex, &end, 16);
                        if (end != byte_hex + 2) {
                            hex_ok = false;
                            break;
                        }
                        out[i] = (uint8_t)v;
                    }
                    if (hex_ok) {
                        return true;
                    }
                }
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    return false;
}

/* ---- Download + verify -------------------------------------------------------- */

jpp_ota_download_status_t jpp_ota_download_update(
    const jpp_ota_release_info_t *info,
    jpp_ota_progress_cb progress_cb, void *progress_ctx,
    char *out_path, size_t out_path_len)
{
    if (info == NULL || out_path == NULL || out_path_len == 0u) {
        return JPP_OTA_DL_ERR_HTTP;
    }

    char path[JPP_OTA_STAGED_PATH_MAX];
    snprintf(path, sizeof(path), "/sd/updates/%s", info->asset_name);
    jpp_make_parent_dirs(path);

    esp_http_client_config_t cfg = {
        .url               = info->asset_url,
        .timeout_ms        = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .transport_type    = HTTP_TRANSPORT_OVER_SSL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return JPP_OTA_DL_ERR_HTTP;
    }
    esp_http_client_set_header(client, "User-Agent", "JPPDOS-OTA");

    int64_t content_len = -1;
    int     status      = 0;
    bool    opened      = false;
    for (unsigned redirect = 0u; redirect <= JPP_OTA_MAX_REDIRECTS; redirect++) {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            esp_http_client_cleanup(client);
            return JPP_OTA_DL_ERR_HTTP;
        }
        opened     = true;
        content_len = esp_http_client_fetch_headers(client);
        status      = esp_http_client_get_status_code(client);

        bool is_redirect = (status == 301 || status == 302 || status == 303 ||
                            status == 307 || status == 308);
        if (!is_redirect) {
            break;
        }
        esp_http_client_close(client);
        opened = false;
        /* Reads the Location header the client already parsed and points
           the handle at it — see esp_http_client_set_redirection()'s doc
           comment, which describes exactly this manual-streaming use case. */
        if (esp_http_client_set_redirection(client) != ESP_OK) {
            esp_http_client_cleanup(client);
            return JPP_OTA_DL_ERR_HTTP;
        }
    }
    if (status != 200) {
        if (opened) {
            esp_http_client_close(client);
        }
        esp_http_client_cleanup(client);
        return JPP_OTA_DL_ERR_HTTP;
    }

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return JPP_OTA_DL_ERR_SD;
    }

    uint8_t *chunk = malloc(JPP_OTA_CHUNK_BYTES);
    if (chunk == NULL) {
        fclose(fp);
        remove(path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return JPP_OTA_DL_ERR_HTTP;
    }

    jpp_heap_monitor_log("ota-download-start");
    size_t total = 0u;
    bool io_ok = true;
    for (;;) {
        int n = esp_http_client_read(client, (char *)chunk, JPP_OTA_CHUNK_BYTES);
        if (n < 0) {
            io_ok = false;
            break;
        }
        if (n == 0) {
            break; /* end of stream (or a stalled read — the size check below catches a short file) */
        }
        if (fwrite(chunk, 1u, (size_t)n, fp) != (size_t)n) {
            io_ok = false;
            break;
        }
        total += (size_t)n;
        if (progress_cb != NULL) {
            progress_cb(total, content_len > 0 ? (size_t)content_len : 0u, progress_ctx);
        }
    }
    jpp_heap_monitor_log("ota-download-stop");

    free(chunk);
    fclose(fp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!io_ok || (content_len > 0 && total != (size_t)content_len)) {
        remove(path);
        return JPP_OTA_DL_ERR_HTTP;
    }

    /* Verify against SHA256SUMS.txt before this file is trusted for anything. */
    char *sums = malloc(JPP_OTA_HTTP_BUF_BYTES);
    if (sums == NULL) {
        remove(path);
        return JPP_OTA_DL_ERR_HASH_FETCH;
    }
    bool got_sums = ota_http_get_body(info->sha_url, sums, JPP_OTA_HTTP_BUF_BYTES);
    uint8_t expected[32];
    bool have_expected = got_sums && find_sha256_line(sums, info->asset_name, expected);
    free(sums);
    if (!have_expected) {
        remove(path);
        return JPP_OTA_DL_ERR_HASH_FETCH;
    }

    uint8_t actual[32];
    if (!sha256_file(path, actual)) {
        remove(path);
        return JPP_OTA_DL_ERR_SD;
    }
    if (memcmp(expected, actual, sizeof(actual)) != 0) {
        ESP_LOGW(TAG, "SHA-256 mismatch for %s", info->asset_name);
        remove(path);
        return JPP_OTA_DL_ERR_HASH_MISMATCH;
    }

    strncpy(out_path, path, out_path_len - 1u);
    out_path[out_path_len - 1u] = '\0';
    return JPP_OTA_DL_OK;
}

/* ---- Pre-flight --------------------------------------------------------------- */

jpp_ota_preflight_status_t jpp_ota_preflight_check(const char *staged_path,
                                                    int battery_pct,
                                                    bool battery_valid)
{
    if (battery_valid && battery_pct >= 0 && battery_pct < JPP_OTA_MIN_BATTERY_PCT) {
        return JPP_OTA_PREFLIGHT_ERR_BATTERY_LOW;
    }

    FILE *fp = fopen(staged_path, "rb");
    if (fp == NULL) {
        return JPP_OTA_PREFLIGHT_ERR_BAD_IMAGE;
    }
    uint8_t magic = 0u;
    size_t got = fread(&magic, 1u, 1u, fp);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fclose(fp);

    if (got != 1u || magic != 0xE9u || size <= 0) {
        return JPP_OTA_PREFLIGHT_ERR_BAD_IMAGE;
    }

    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory == NULL || (size_t)size > factory->size) {
        return JPP_OTA_PREFLIGHT_ERR_TOO_LARGE;
    }
    return JPP_OTA_PREFLIGHT_OK;
}

/* ---- Flash + reboot ------------------------------------------------------------ */

jpp_ota_flash_error_t jpp_ota_flash_and_reboot(const char *staged_path,
                                                jpp_ota_progress_cb progress_cb,
                                                void *progress_ctx)
{
    FILE *fp = fopen(staged_path, "rb");
    if (fp == NULL) {
        return JPP_OTA_FLASH_ERR_OPEN;
    }
    fseek(fp, 0, SEEK_END);
    long file_size_l = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (file_size_l <= 0) {
        fclose(fp);
        return JPP_OTA_FLASH_ERR_OPEN;
    }
    size_t file_size = (size_t)file_size_l;

    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory == NULL || file_size > factory->size) {
        fclose(fp);
        return JPP_OTA_FLASH_ERR_NO_PARTITION;
    }

    uint8_t *buf = malloc(JPP_OTA_CHUNK_BYTES);
    if (buf == NULL) {
        fclose(fp);
        return JPP_OTA_FLASH_ERR_OPEN;
    }

    /* No further use for the radio, and one less thing contending for heap
       and flash-cache bus time during the critical section below. */
    wifi_disconnect();

    size_t erase_len = ((file_size + JPP_OTA_ERASE_ALIGN - 1u) / JPP_OTA_ERASE_ALIGN)
                        * JPP_OTA_ERASE_ALIGN;
    if (erase_len > factory->size) {
        erase_len = factory->size;
    }

    jpp_heap_monitor_log("ota-flash-start");

    /* ---- Point of no return: `factory` (the partition we are executing
       from) no longer reliably holds a bootable image until every write
       below completes and the readback verify passes. No fallback partition
       exists on this device — see AGENTS.md "Firmware update (OTA)". ---- */
    if (esp_partition_erase_range(factory, 0, erase_len) != ESP_OK) {
        free(buf);
        fclose(fp);
        return JPP_OTA_FLASH_ERR_ERASE;
    }

    size_t offset = 0u;
    size_t n;
    bool write_ok = true;
    while ((n = fread(buf, 1u, JPP_OTA_CHUNK_BYTES, fp)) > 0u) {
        if (esp_partition_write(factory, offset, buf, n) != ESP_OK) {
            write_ok = false;
            break;
        }
        offset += n;
        if (progress_cb != NULL) {
            progress_cb(offset, file_size * 2u, progress_ctx); /* first half of the bar */
        }
    }
    fclose(fp);
    if (!write_ok || offset != file_size) {
        free(buf);
        return JPP_OTA_FLASH_ERR_WRITE;
    }

    /* Read back and byte-compare against the staged (already SHA-256
       verified) file — the integrity guarantee esp_ota_end() would give,
       reimplemented because esp_ota_ops cannot target this partition. */
    fp = fopen(staged_path, "rb");
    if (fp == NULL) {
        free(buf);
        return JPP_OTA_FLASH_ERR_VERIFY;
    }
    uint8_t *readback = malloc(JPP_OTA_CHUNK_BYTES);
    if (readback == NULL) {
        free(buf);
        fclose(fp);
        return JPP_OTA_FLASH_ERR_VERIFY;
    }

    offset = 0u;
    bool verify_ok = true;
    while ((n = fread(buf, 1u, JPP_OTA_CHUNK_BYTES, fp)) > 0u) {
        if (esp_partition_read(factory, offset, readback, n) != ESP_OK ||
            memcmp(buf, readback, n) != 0) {
            verify_ok = false;
            break;
        }
        offset += n;
        if (progress_cb != NULL) {
            progress_cb(file_size + offset, file_size * 2u, progress_ctx); /* second half */
        }
    }
    fclose(fp);
    free(buf);
    free(readback);
    jpp_heap_monitor_log("ota-flash-stop");

    if (!verify_ok || offset != file_size) {
        return JPP_OTA_FLASH_ERR_VERIFY;
    }

    remove(staged_path);
    ESP_LOGI(TAG, "OTA flash verified, rebooting");
    esp_restart();
    return JPP_OTA_FLASH_ERR_OPEN; /* unreachable */
}
