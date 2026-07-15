#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPP_LED_STATUS_OK = 0,
    JPP_LED_STATUS_INVALID_STATE,
} jpp_led_status_t;

/* Initialise the RMT TX channel driving the onboard WS2812 pixel.  Idempotent;
   also called lazily by jpp_led_set_color()/jpp_led_off() on first use. */
jpp_led_status_t jpp_led_init(void);

/* Set the onboard WS2812 pixel to an RGB color (0-255 per channel). */
jpp_led_status_t jpp_led_set_color(uint8_t r, uint8_t g, uint8_t b);

/* Turn the onboard LED off. Equivalent to jpp_led_set_color(0, 0, 0). */
jpp_led_status_t jpp_led_off(void);

#ifdef __cplusplus
}
#endif
