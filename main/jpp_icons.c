#include "jpp_icons.h"
#include "ssd1306.h"

#include <string.h>

/* ---- Bitmap data ---------------------------------------------------------- */

/* Wi-Fi arc (8×8, column-major, bit-0=top). Drawn from top to bottom. */
static const uint8_t ICON_WIFI[8]      = { 0x00, 0x00, 0x0E, 0x10, 0x26, 0x28, 0x2A, 0x00 };

/* Check-mark ✓ (8×8, row-major, MSB = leftmost pixel).
 * Pixel map (X = lit):
 *   row1: col7              .......X
 *   row2: col6              ......X.
 *   row3: col5              .....X..
 *   row4: col0, col4        X...X...
 *   row5: col1, col3        .X.X....
 *   row6: col2              ..X.....
 */
static const uint8_t ICON_CHECKMARK[8] = { 0x00, 0x01, 0x02, 0x04, 0x88, 0x50, 0x20, 0x00 };

static const uint8_t ICON_BLANK[8]     = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

/* ---- Battery outline frames (column-major) -------------------------------- */

/* Body outline of the left 8 columns of the 12×6 battery (6px tall, 1px pad top/bottom). */
static const uint8_t BAT_FRAME_L[8] = { 0x00, 0xFF, 0x80, 0x80, 0x80, 0x80, 0xFF, 0x00 };
/* Right 4 columns: outer wall + terminal nub (rows 2-5). */
static const uint8_t BAT_FRAME_R[8] = { 0x00, 0xC0, 0x70, 0x70, 0x70, 0x70, 0xC0, 0x00 };

/* ---- Public API ----------------------------------------------------------- */

void jpp_icon_draw(uint8_t page, uint8_t col, jpp_icon_id_t icon)
{
    const uint8_t *bmp;
    switch (icon) {
    case JPP_ICON_WIFI:      bmp = ICON_WIFI;      break;
    case JPP_ICON_CHECKMARK: bmp = ICON_CHECKMARK; break;
    default:                 bmp = ICON_BLANK;      break;
    }
    ssd1306_draw_bitmap_8x8(page, col, bmp, false);
}

void jpp_icon_battery_draw(uint8_t page, uint8_t lft_col, int pct)
{
    uint8_t lft[8], rgt[8];
    memcpy(lft, BAT_FRAME_L, 8);
    memcpy(rgt, BAT_FRAME_R, 8);

    if (pct > 0) {
        if (pct > 100) { pct = 100; }
        int fill_w    = (pct * 8 + 50) / 100;   /* 0-8 interior pixels */
        int fill_left = fill_w < 7 ? fill_w : 7;
        /* Interior cols 1-7 in lft = bits 6-0; fill left-to-right from MSB. */
        uint8_t lmask = (uint8_t)(((1u << fill_left) - 1u) << (7u - fill_left));
        uint8_t rmask = (fill_w > 7) ? 0x80u : 0u; /* col 8 = bit 7 of rgt */
        for (int row = 2; row <= 5; row++) {
            lft[row] |= lmask;
            rgt[row] |= rmask;
        }
    }

    ssd1306_draw_bitmap_8x8(page, lft_col,       lft, false);
    ssd1306_draw_bitmap_8x8(page, lft_col + 8u,  rgt, false);
}
