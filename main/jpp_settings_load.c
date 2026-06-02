#include "jpp_settings_load.h"

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
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return JPP_SETTINGS_PAYLOAD_MISSING;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(f);
        return JPP_SETTINGS_PAYLOAD_CORRUPT;
    }
    fread(buf, 1u, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return JPP_SETTINGS_PAYLOAD_CORRUPT;
    }
    cJSON *ver = cJSON_GetObjectItem(root, "schema_version");
    jpp_settings_payload_status_t status = JPP_SETTINGS_PAYLOAD_CORRUPT;
    if (cJSON_IsNumber(ver)) {
        int v = (int)ver->valuedouble;
        if (v == 2) {
            status = JPP_SETTINGS_PAYLOAD_VALID;
        } else if (v == 1) {
            status = JPP_SETTINGS_PAYLOAD_MIGRATION_REQUIRED;
        }
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
    if (!file_exists(SETTINGS_PATH)) {
        return false;
    }
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    fread(buf, 1u, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);

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
