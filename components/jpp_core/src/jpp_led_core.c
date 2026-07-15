#include "../include/jpp_led_core.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"

/* jpp_core cannot directly include main/jpp_hw_config.h, so the pin constant
   is duplicated here as a default that matches the header value (same
   convention as jpp_buzzer_core.c). */
#ifndef JPP_HW_LED_GPIO
#define JPP_HW_LED_GPIO 8
#endif

#define LED_RMT_RESOLUTION_HZ 10000000u  /* 10 MHz, 1 tick = 0.1 us */

static const char *TAG = "jpp_led";

/* ---- WS2812 composite encoder (bytes + reset pulse) ---------------------- */
/*
 * WS2812 needs one bit-pattern encoder for the GRB data bytes followed by a
 * >=50us low reset pulse before the pixel latches.  rmt_bytes_encoder_t only
 * emits the data; the reset pulse is a second, tiny copy-encoder session.
 * This composite mirrors Espressif's reference led_strip RMT encoder.
 */
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} led_strip_encoder_t;

static size_t led_strip_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                const void *primary_data, size_t data_size,
                                rmt_encode_state_t *ret_state)
{
    led_strip_encoder_t *led_encoder = (led_strip_encoder_t *)encoder;
    rmt_encoder_handle_t bytes_encoder = led_encoder->bytes_encoder;
    rmt_encoder_handle_t copy_encoder  = led_encoder->copy_encoder;
    rmt_encode_state_t session_state = (rmt_encode_state_t)0;
    rmt_encode_state_t state = (rmt_encode_state_t)0;
    size_t encoded_symbols = 0u;

    if (led_encoder->state == 0) {
        encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, primary_data,
                                                  data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state = (rmt_encode_state_t)(state | RMT_ENCODING_MEM_FULL);
            *ret_state = state;
            return encoded_symbols;
        }
    }
    if (led_encoder->state == 1) {
        encoded_symbols += copy_encoder->encode(copy_encoder, channel, &led_encoder->reset_code,
                                                 sizeof(led_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 0;
            state = (rmt_encode_state_t)(state | RMT_ENCODING_COMPLETE);
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state = (rmt_encode_state_t)(state | RMT_ENCODING_MEM_FULL);
        }
    }
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t led_strip_encoder_reset(rmt_encoder_t *encoder)
{
    led_strip_encoder_t *led_encoder = (led_strip_encoder_t *)encoder;
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = 0;
    return ESP_OK;
}

static esp_err_t led_strip_encoder_del(rmt_encoder_t *encoder)
{
    led_strip_encoder_t *led_encoder = (led_strip_encoder_t *)encoder;
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

static esp_err_t new_led_strip_encoder(rmt_encoder_handle_t *ret_encoder)
{
    led_strip_encoder_t *led_encoder = calloc(1, sizeof(led_strip_encoder_t));
    if (led_encoder == NULL) {
        return ESP_ERR_NO_MEM;
    }
    led_encoder->base.encode = led_strip_encode;
    led_encoder->base.del    = led_strip_encoder_del;
    led_encoder->base.reset  = led_strip_encoder_reset;

    /* WS2812 timing (ticks at 10 MHz, 1 tick = 0.1us): T0H=0.3us, T0L=0.9us,
       T1H=0.9us, T1L=0.3us, MSB first. */
    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = {
            .duration0 = 3, .level0 = 1,
            .duration1 = 9, .level1 = 0,
        },
        .bit1 = {
            .duration0 = 9, .level0 = 1,
            .duration1 = 3, .level1 = 0,
        },
        .flags.msb_first = 1,
    };
    esp_err_t err = rmt_new_bytes_encoder(&bytes_cfg, &led_encoder->bytes_encoder);
    if (err != ESP_OK) {
        free(led_encoder);
        return err;
    }

    rmt_copy_encoder_config_t copy_cfg;
    memset(&copy_cfg, 0, sizeof(copy_cfg));
    err = rmt_new_copy_encoder(&copy_cfg, &led_encoder->copy_encoder);
    if (err != ESP_OK) {
        rmt_del_encoder(led_encoder->bytes_encoder);
        free(led_encoder);
        return err;
    }

    /* >=50us reset pulse, split across the symbol's two low half-periods. */
    uint32_t reset_ticks = (LED_RMT_RESOLUTION_HZ / 1000000u) * 50u / 2u;
    led_encoder->reset_code = (rmt_symbol_word_t){
        .duration0 = reset_ticks, .level0 = 0,
        .duration1 = reset_ticks, .level1 = 0,
    };

    *ret_encoder = &led_encoder->base;
    return ESP_OK;
}

/* ---- Module state ---------------------------------------------------------- */

static bool                  s_initialized = false;
static rmt_channel_handle_t  s_chan        = NULL;
static rmt_encoder_handle_t  s_encoder     = NULL;

jpp_led_status_t jpp_led_init(void)
{
    if (s_initialized) {
        return JPP_LED_STATUS_OK;
    }

    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num          = (gpio_num_t)JPP_HW_LED_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64u,
        .trans_queue_depth = 4u,
    };
    if (rmt_new_tx_channel(&chan_cfg, &s_chan) != ESP_OK) {
        ESP_LOGW(TAG, "rmt_new_tx_channel failed");
        return JPP_LED_STATUS_INVALID_STATE;
    }
    if (new_led_strip_encoder(&s_encoder) != ESP_OK) {
        ESP_LOGW(TAG, "led strip encoder create failed");
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return JPP_LED_STATUS_INVALID_STATE;
    }
    if (rmt_enable(s_chan) != ESP_OK) {
        ESP_LOGW(TAG, "rmt_enable failed");
        rmt_del_encoder(s_encoder);
        rmt_del_channel(s_chan);
        s_chan    = NULL;
        s_encoder = NULL;
        return JPP_LED_STATUS_INVALID_STATE;
    }

    s_initialized = true;
    return JPP_LED_STATUS_OK;
}

jpp_led_status_t jpp_led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized) {
        jpp_led_status_t st = jpp_led_init();
        if (st != JPP_LED_STATUS_OK) {
            return st;
        }
    }

    /* WS2812 wire order is GRB, not RGB. */
    uint8_t pixel[3] = { g, r, b };
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    if (rmt_transmit(s_chan, s_encoder, pixel, sizeof(pixel), &tx_cfg) != ESP_OK) {
        return JPP_LED_STATUS_INVALID_STATE;
    }
    rmt_tx_wait_all_done(s_chan, pdMS_TO_TICKS(100));
    return JPP_LED_STATUS_OK;
}

jpp_led_status_t jpp_led_off(void)
{
    return jpp_led_set_color(0u, 0u, 0u);
}
