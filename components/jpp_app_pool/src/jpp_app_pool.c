#include "jpp_app_pool.h"

#include <stdint.h>

#include "esp_log.h"

static const char *TAG = "app_pool";

/* 16-byte aligned: satisfies RISC-V instruction fetch for native code and the
   MicroPython GC heap's pointer-alignment expectations. */
static uint8_t s_pool[JPP_APP_POOL_BYTES] __attribute__((aligned(16)));
static bool    s_in_use = false;

/* Bump pointer for jpp_app_pool_alloc(); reset on every acquire/release. */
static size_t      s_used  = 0u;
static const char *s_owner = NULL;

void *jpp_app_pool_acquire_as(const char *owner, size_t need_bytes, size_t *out_size)
{
    if (s_in_use) {
        ESP_LOGE(TAG, "acquire(%s): pool already held by %s",
                 (owner != NULL) ? owner : "?",
                 (s_owner != NULL) ? s_owner : "?");
        return NULL;
    }
    if (need_bytes > JPP_APP_POOL_BYTES) {
        ESP_LOGE(TAG, "acquire: need %zu > pool %u bytes",
                 need_bytes, (unsigned)JPP_APP_POOL_BYTES);
        return NULL;
    }
    s_in_use = true;
    s_used   = 0u;
    s_owner  = (owner != NULL) ? owner : "app";
    if (out_size != NULL) { *out_size = JPP_APP_POOL_BYTES; }
    return s_pool;
}

void *jpp_app_pool_acquire(size_t need_bytes, size_t *out_size)
{
    return jpp_app_pool_acquire_as("app", need_bytes, out_size);
}

void *jpp_app_pool_alloc(size_t bytes, size_t align)
{
    if (!s_in_use) {
        ESP_LOGE(TAG, "alloc: pool not held");
        return NULL;
    }
    if (align < 4u) { align = 4u; }
    size_t start = (s_used + (align - 1u)) & ~(align - 1u);
    if (start > JPP_APP_POOL_BYTES || bytes > JPP_APP_POOL_BYTES - start) {
        ESP_LOGE(TAG, "alloc(%s): %zu bytes (align %zu) exceeds %zu remaining",
                 (s_owner != NULL) ? s_owner : "?", bytes, align,
                 JPP_APP_POOL_BYTES - s_used);
        return NULL;
    }
    s_used = start + bytes;
    return s_pool + start;
}

size_t jpp_app_pool_avail(void)
{
    return s_in_use ? (JPP_APP_POOL_BYTES - s_used) : (size_t)JPP_APP_POOL_BYTES;
}

void jpp_app_pool_release(void)
{
    s_in_use = false;
    s_used   = 0u;
    s_owner  = NULL;
}

size_t jpp_app_pool_size(void)
{
    return JPP_APP_POOL_BYTES;
}

bool jpp_app_pool_in_use(void)
{
    return s_in_use;
}

const char *jpp_app_pool_owner(void)
{
    return s_in_use ? s_owner : NULL;
}
