#include "jpp_backup_restore.h"
#include "jpp_settings_load.h"
#include "jpp_lrv.h"
#include "jpp_buzzer_core.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

static const char *TAG = "backup_restore";

#define NS_TIME   "jpp_time"
#define NS_POWER  "jpp_power"
#define NS_WEBDAV "jpp_webdav"
#define NS_SOUND  "jpp_sound"
#define NS_USER   "jpp_user"

bool jpp_backup_apply_json(const char *json_buf, char *msg, size_t msg_len)
{
#define FAIL(m) do { if (msg) { snprintf(msg, msg_len, "%s", (m)); } \
                     cJSON_Delete(root); return false; } while (0)

    cJSON *root = cJSON_Parse(json_buf);
    if (root == NULL) {
        if (msg) { snprintf(msg, msg_len, "Error: invalid JSON"); }
        return false;
    }

    cJSON *sig = cJSON_GetObjectItem(root, "jppdos_backup");
    if (!cJSON_IsNumber(sig) || (int)sig->valuedouble != 1) {
        FAIL("Error: not a backup file");
    }

    /* settings.json */
    cJSON *settings_obj = cJSON_GetObjectItem(root, "settings");
    if (cJSON_IsObject(settings_obj)) {
        char *s = cJSON_PrintUnformatted(settings_obj);
        if (s != NULL) { write_settings(s); free(s); }
    }

    nvs_handle_t h;

    /* jpp_time */
    cJSON *nvs_time = cJSON_GetObjectItem(root, "nvs_time");
    if (cJSON_IsObject(nvs_time) &&
        nvs_open(NS_TIME, NVS_READWRITE, &h) == ESP_OK) {
        cJSON *v;
        v = cJSON_GetObjectItem(nvs_time, "ntp_en");
        if (cJSON_IsNumber(v)) { nvs_set_u8(h, "ntp_en", (uint8_t)(int)v->valuedouble); }
        v = cJSON_GetObjectItem(nvs_time, "ntp_host");
        if (cJSON_IsString(v) && v->valuestring) { nvs_set_str(h, "ntp_host", v->valuestring); }
        v = cJSON_GetObjectItem(nvs_time, "tz_h");
        if (cJSON_IsNumber(v)) { nvs_set_i8(h, "tz_h", (int8_t)(int)v->valuedouble); }
        nvs_commit(h); nvs_close(h);
    }

    /* jpp_power */
    cJSON *nvs_power = cJSON_GetObjectItem(root, "nvs_power");
    if (cJSON_IsObject(nvs_power) &&
        nvs_open(NS_POWER, NVS_READWRITE, &h) == ESP_OK) {
        cJSON *v;
        v = cJSON_GetObjectItem(nvs_power, "dim_s");
        if (cJSON_IsNumber(v)) { nvs_set_i32(h, "dim_s", (int32_t)v->valuedouble); }
        v = cJSON_GetObjectItem(nvs_power, "poweroff_s");
        if (cJSON_IsNumber(v)) { nvs_set_i32(h, "poweroff_s", (int32_t)v->valuedouble); }
        nvs_commit(h); nvs_close(h);
    }

    /* jpp_webdav */
    cJSON *nvs_webdav = cJSON_GetObjectItem(root, "nvs_webdav");
    if (cJSON_IsObject(nvs_webdav) &&
        nvs_open(NS_WEBDAV, NVS_READWRITE, &h) == ESP_OK) {
        cJSON *v;
        v = cJSON_GetObjectItem(nvs_webdav, "pass_static");
        if (cJSON_IsNumber(v)) { nvs_set_u8(h, "pass_static", (uint8_t)(int)v->valuedouble); }
        v = cJSON_GetObjectItem(nvs_webdav, "static_pass");
        if (cJSON_IsString(v) && v->valuestring) { nvs_set_str(h, "static_pass", v->valuestring); }
        nvs_commit(h); nvs_close(h);
    }

    /* jpp_sound */
    cJSON *nvs_sound = cJSON_GetObjectItem(root, "nvs_sound");
    if (cJSON_IsObject(nvs_sound) &&
        nvs_open(NS_SOUND, NVS_READWRITE, &h) == ESP_OK) {
        cJSON *v;
        v = cJSON_GetObjectItem(nvs_sound, "buzzer_vol");
        if (cJSON_IsNumber(v)) { nvs_set_u8(h, "buzzer_vol", (uint8_t)(int)v->valuedouble); }
        v = cJSON_GetObjectItem(nvs_sound, "startup_jingle");
        if (cJSON_IsNumber(v) && (int)v->valuedouble >= 0 &&
            (int)v->valuedouble < (int)JPP_STARTUP_JINGLE_COUNT) {
            nvs_set_u8(h, "startup_jingle", (uint8_t)(int)v->valuedouble);
        }
        nvs_commit(h); nvs_close(h);
    }

    /* jpp_user */
    cJSON *nvs_user = cJSON_GetObjectItem(root, "nvs_user");
    if (cJSON_IsObject(nvs_user) &&
        nvs_open(NS_USER, NVS_READWRITE, &h) == ESP_OK) {
        cJSON *v = cJSON_GetObjectItem(nvs_user, "username");
        if (cJSON_IsString(v) && v->valuestring) { nvs_set_str(h, "username", v->valuestring); }
        nvs_commit(h); nvs_close(h);
    }

    /* jpp_lrv — encrypted blob (base64-encoded in backup) */
    cJSON *nvs_lrv = cJSON_GetObjectItem(root, "nvs_lrv");
    if (cJSON_IsObject(nvs_lrv)) {
        cJSON *enc_item = cJSON_GetObjectItem(nvs_lrv, "lrv_enc");
        if (cJSON_IsString(enc_item) && enc_item->valuestring) {
            const char *b64     = enc_item->valuestring;
            size_t      b64_len = strlen(b64);
            size_t      bin_max = (b64_len / 4u) * 3u + 4u;
            uint8_t    *bin     = malloc(bin_max);
            if (bin != NULL) {
                size_t olen = 0u;
                if (mbedtls_base64_decode(bin, bin_max, &olen,
                                           (const unsigned char *)b64, b64_len) == 0) {
                    jpp_lrv_store_encrypted_blob(bin, olen);
                }
                free(bin);
            }
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "RESTORE_OK");
    return true;

#undef FAIL
}
