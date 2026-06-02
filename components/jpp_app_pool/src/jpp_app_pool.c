#include "jpp_app_pool.h"

#include "esp_log.h"

static const char *TAG = "app_pool";

/* 16-byte aligned: satisfies RISC-V instruction fetch for native code and the
   MicroPython GC heap's pointer-alignment expectations. */
static uint8_t s_pool[JPP_APP_POOL_BYTES] __attribute__((aligned(16)));
static bool    s_in_use = false;

void *jpp_app_pool_acquire(size_t need_bytes, size_t *out_size)
{
    if (s_in_use) {
        ESP_LOGE(TAG, "acquire: pool already in use");
        return NULL;
    }
    if (need_bytes > JPP_APP_POOL_BYTES) {
        ESP_LOGE(TAG, "acquire: need %zu > pool %u bytes",
                 need_bytes, (unsigned)JPP_APP_POOL_BYTES);
        return NULL;
    }
    s_in_use = true;
    if (out_size != NULL) { *out_size = JPP_APP_POOL_BYTES; }
    return s_pool;
}

void jpp_app_pool_release(void)
{
    s_in_use = false;
}

size_t jpp_app_pool_size(void)
{
    return JPP_APP_POOL_BYTES;
}

bool jpp_app_pool_in_use(void)
{
    return s_in_use;
}
