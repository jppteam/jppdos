#pragma once

#include "mpconfigport.h"  /* mp_uint_t must be defined before use below */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

static inline void mp_hal_delay_ms(mp_uint_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static inline void mp_hal_delay_us(mp_uint_t us)
{
    if (us < 1000u) {
        esp_rom_delay_us((uint32_t)us);
    } else {
        vTaskDelay(pdMS_TO_TICKS((us + 999u) / 1000u));
    }
}

static inline mp_uint_t mp_hal_ticks_ms(void)
{
    return (mp_uint_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static inline mp_uint_t mp_hal_ticks_us(void)
{
    return (mp_uint_t)esp_timer_get_time();
}

void mp_hal_set_interrupt_char(int c);
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len);
