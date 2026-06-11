#include "jpp_settings_load.h"
#include "jpp_file_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "settings_load";

bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

jpp_settings_payload_status_t probe_settings_payload(const char *path)
{
    if (!file_exists(path)) {
        return JPP_SETTINGS_PAYLOAD_MISSING;
    }
    char *buf = jpp_read_file(path, NULL);
    if (buf == NULL) {
        return JPP_SETTINGS_PAYLOAD_CORRUPT;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return JPP_SETTINGS_PAYLOAD_CORRUPT;
    }
    /* Any schema other than the current v2 is treated as corrupt: the boot
       path rewrites defaults and logs SETTINGS_RESET. */
    cJSON *ver = cJSON_GetObjectItem(root, "schema_version");
    jpp_settings_payload_status_t status = JPP_SETTINGS_PAYLOAD_CORRUPT;
    if (cJSON_IsNumber(ver) && (int)ver->valuedouble == 2) {
        status = JPP_SETTINGS_PAYLOAD_VALID;
    }
    cJSON_Delete(root);
    return status;
}

/* Atomic write: stage via .tmp then rename over the main file. */
void write_settings(const char *json)
{
    FILE *f = fopen(SETTINGS_TMP_PATH, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Cannot open %s for write", SETTINGS_TMP_PATH);
        return;
    }
    fputs(json, f);
    fclose(f);
    remove(SETTINGS_PATH);
    rename(SETTINGS_TMP_PATH, SETTINGS_PATH);
}

bool read_force_recovery(void)
{
    char *buf = jpp_read_file(SETTINGS_PATH, NULL);
    if (buf == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return false;
    }
    cJSON *policy   = cJSON_GetObjectItem(root, "policy");
    cJSON *recovery = policy   ? cJSON_GetObjectItem(policy,   "recovery")      : NULL;
    cJSON *force    = recovery ? cJSON_GetObjectItem(recovery, "force_recovery") : NULL;
    bool result = cJSON_IsTrue(force);
    cJSON_Delete(root);
    return result;
}

void jpp_settings_read_wifi(char *ssid, size_t ssid_size,
                            char *password, size_t password_size)
{
    ssid[0]     = '\0';
    password[0] = '\0';

    char *buf = jpp_read_file(SETTINGS_PATH, NULL);
    if (buf == NULL) return;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) return;

    cJSON *policy = cJSON_GetObjectItem(root, "policy");
    cJSON *wifi   = policy ? cJSON_GetObjectItem(policy, "wifi") : NULL;
    cJSON *j_ssid = wifi   ? cJSON_GetObjectItem(wifi, "preferred_ssid") : NULL;
    cJSON *j_pass = wifi   ? cJSON_GetObjectItem(wifi, "password")       : NULL;

    if (cJSON_IsString(j_ssid) && j_ssid->valuestring) {
        strncpy(ssid, j_ssid->valuestring, ssid_size - 1u);
        ssid[ssid_size - 1u] = '\0';
    }
    if (cJSON_IsString(j_pass) && j_pass->valuestring) {
        strncpy(password, j_pass->valuestring, password_size - 1u);
        password[password_size - 1u] = '\0';
    }
    cJSON_Delete(root);
}

void jpp_settings_save_wifi(const char *ssid, const char *password)
{
    char *buf = jpp_read_file(SETTINGS_PATH, NULL);
    if (buf == NULL) return;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) return;

    /* Navigate / create policy.wifi. */
    cJSON *policy = cJSON_GetObjectItem(root, "policy");
    if (!cJSON_IsObject(policy)) {
        policy = cJSON_AddObjectToObject(root, "policy");
    }
    cJSON *wifi = cJSON_GetObjectItem(policy, "wifi");
    if (!cJSON_IsObject(wifi)) {
        wifi = cJSON_AddObjectToObject(policy, "wifi");
    }

    cJSON_DeleteItemFromObject(wifi, "preferred_ssid");
    cJSON_DeleteItemFromObject(wifi, "password");
    cJSON_AddStringToObject(wifi, "preferred_ssid", ssid ? ssid : "");
    cJSON_AddStringToObject(wifi, "password",       password ? password : "");

    char *out_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out_str == NULL) return;

    /* The remove + rename in write_settings works on every filesystem and
       leaves a recoverable temp file on failure. */
    write_settings(out_str);
    free(out_str);
    ESP_LOGI(TAG, "WIFI: credentials saved (ssid=\"%s\")", ssid ? ssid : "");
}
