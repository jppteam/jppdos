#pragma once

/*
 * jpp_icons — centralised icon bitmaps and draw helpers for the SSD1306.
 *
 * All 8×8 bitmaps and the 12×8 battery icon live here. Use jpp_icon_draw()
 * for single-call placement; jpp_icon_battery_draw() for the battery which
 * needs two adjacent 8×8 halves. Bitmaps are column-major: one byte per
 * column, bit-0 = top pixel, bit-7 = bottom pixel.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPP_ICON_NONE      = 0, /* erase: draws a blank 8×8 block */
    JPP_ICON_WIFI      = 1,
    JPP_ICON_CHECKMARK = 2,
} jpp_icon_id_t;

/* Draw an 8×8 icon at (page, col). JPP_ICON_NONE clears the 8×8 area. */
void jpp_icon_draw(uint8_t page, uint8_t col, jpp_icon_id_t icon);

/* Draw the 12×6 battery icon (6px tall, centred in an 8px page row).
   Left half at (page, lft_col), right half at (page, lft_col + 8). */
void jpp_icon_battery_draw(uint8_t page, uint8_t lft_col, int pct);

#ifdef __cplusplus
}
#endif
