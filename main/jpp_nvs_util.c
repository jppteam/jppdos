#include "jpp_nvs_util.h"

#include "nvs.h"

uint8_t jpp_nvs_get_u8(const char *ns, const char *key, uint8_t fallback)
{
    nvs_handle_t h;
    uint8_t value = fallback;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, key, &value) != ESP_OK) {
            value = fallback;
        }
        nvs_close(h);
    }
    return value;
}

int32_t jpp_nvs_get_i32(const char *ns, const char *key, int32_t fallback)
{
    nvs_handle_t h;
    int32_t value = fallback;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, key, &value) != ESP_OK) {
            value = fallback;
        }
        nvs_close(h);
    }
    return value;
}

bool jpp_nvs_get_str(const char *ns, const char *key, char *out, size_t out_len)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        size_t len = out_len;
        ok = (nvs_get_str(h, key, out, &len) == ESP_OK);
        nvs_close(h);
    }
    return ok;
}

void jpp_nvs_set_u8(const char *ns, const char *key, uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

void jpp_nvs_set_i32(const char *ns, const char *key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

void jpp_nvs_set_str(const char *ns, const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

void jpp_nvs_erase_key(const char *ns, const char *key)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
}
