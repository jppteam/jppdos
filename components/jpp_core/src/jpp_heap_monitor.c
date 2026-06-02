#include "../include/jpp_heap_monitor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "heap_mon";

/* Heap pressure level for hysteresis in the sampler. */
typedef enum {
    HEAP_LEVEL_OK = 0,
    HEAP_LEVEL_WARN,
    HEAP_LEVEL_CRIT,
} heap_level_t;

static bool s_started = false;

/* ---- Failed-allocation callback ----------------------------------------- */

/* Fires synchronously in the context of the failing allocation.  Must be
   lightweight and must NOT allocate.  ESP_LOG writes to a stack buffer (no
   heap), so it is safe here. */
static void heap_failed_cb(size_t size, uint32_t caps, const char *fn)
{
    ESP_LOGE(TAG,
             "ALLOC FAILED: %u bytes caps=0x%lx in %s | free=%u largest=%u",
             (unsigned)size, (unsigned long)caps,
             (fn != NULL) ? fn : "?",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

/* ---- Periodic sampler --------------------------------------------------- */

static void emit(const char *label, size_t free8, size_t min8, size_t largest)
{
    ESP_LOGW(TAG, "heap @%s: free=%u min=%u largest=%u",
             label, (unsigned)free8, (unsigned)min8, (unsigned)largest);
}

static void monitor_task(void *arg)
{
    (void)arg;
    heap_level_t level = HEAP_LEVEL_OK;

    for (;;) {
        size_t free8    = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        size_t min8     = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

        heap_level_t now;
        if (free8 < JPP_HEAP_MON_CRIT_BYTES) {
            now = HEAP_LEVEL_CRIT;
        } else if (free8 < JPP_HEAP_MON_WARN_BYTES) {
            now = HEAP_LEVEL_WARN;
        } else {
            now = HEAP_LEVEL_OK;
        }

        /* Log every interval while under pressure, once on recovery to OK, and
           never while comfortably OK (avoids steady-state log spam). */
        if (now == HEAP_LEVEL_CRIT) {
            ESP_LOGE(TAG, "CRITICAL: free=%u min=%u largest=%u (frame allocs may fail)",
                     (unsigned)free8, (unsigned)min8, (unsigned)largest);
            /* On the first entry into CRIT, dump a fuller breakdown to attribute
               the pressure.  heap_caps_print_heap_info walks the heap (no alloc). */
            if (level != HEAP_LEVEL_CRIT) {
                heap_caps_print_heap_info(MALLOC_CAP_8BIT);
            }
        } else if (now == HEAP_LEVEL_WARN) {
            emit("warn", free8, min8, largest);
        } else if (level != HEAP_LEVEL_OK) {
            ESP_LOGI(TAG, "recovered: free=%u min=%u largest=%u",
                     (unsigned)free8, (unsigned)min8, (unsigned)largest);
        }

        level = now;
        vTaskDelay(pdMS_TO_TICKS(JPP_HEAP_MON_INTERVAL_MS));
    }
}

/* ---- Public API --------------------------------------------------------- */

bool jpp_heap_monitor_init(void)
{
    if (s_started) { return true; }

    esp_err_t err = heap_caps_register_failed_alloc_callback(heap_failed_cb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed-alloc callback not registered: %s",
                 esp_err_to_name(err));
        /* Non-fatal — continue with the sampler. */
    }

    /* ESP-IDF stack depth is in bytes.  3 KB covers ESP_LOG formatting plus the
       heap_caps_print_heap_info dump emitted on entry to CRIT. */
    BaseType_t ok = xTaskCreate(monitor_task, "heap_mon", 3072, NULL,
                                tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "monitor task create failed");
        return false;
    }

    s_started = true;
    jpp_heap_monitor_log("monitor-start");
    return true;
}

void jpp_heap_monitor_log(const char *label)
{
    emit((label != NULL) ? label : "?",
         heap_caps_get_free_size(MALLOC_CAP_8BIT),
         heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
