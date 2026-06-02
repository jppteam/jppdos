#pragma once

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
