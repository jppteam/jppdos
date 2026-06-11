#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Single-key NVS accessors. Each call opens the namespace, performs the one
   operation, commits (for writes), and closes — for the common "one setting,
   one key" case. Multi-key sections should keep a single open/commit/close. */

/* Reads return fallback when the namespace or key is absent. */
uint8_t jpp_nvs_get_u8(const char *ns, const char *key, uint8_t fallback);
int32_t jpp_nvs_get_i32(const char *ns, const char *key, int32_t fallback);

/* Returns true and fills out on success; leaves out untouched otherwise. */
bool jpp_nvs_get_str(const char *ns, const char *key, char *out, size_t out_len);

void jpp_nvs_set_u8(const char *ns, const char *key, uint8_t value);
void jpp_nvs_set_i32(const char *ns, const char *key, int32_t value);
void jpp_nvs_set_str(const char *ns, const char *key, const char *value);
void jpp_nvs_erase_key(const char *ns, const char *key);
