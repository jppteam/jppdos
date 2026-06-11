#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "jpp_settings_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_PATH     "/data/settings.json"
#define SETTINGS_TMP_PATH "/data/settings.json.tmp"

bool                         file_exists(const char *path);
jpp_settings_payload_status_t probe_settings_payload(const char *path);
void                         write_settings(const char *json);
bool                         read_force_recovery(void);

/* Read policy.wifi credentials from settings.json. Outputs are always
   NUL-terminated; empty strings when the file or fields are absent. */
void jpp_settings_read_wifi(char *ssid, size_t ssid_size,
                            char *password, size_t password_size);

/* Persist policy.wifi credentials into settings.json
   (load, modify, then save through the atomic write_settings path). */
void jpp_settings_save_wifi(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
