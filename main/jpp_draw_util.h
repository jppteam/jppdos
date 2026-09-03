#pragma once

#include "ssd1306.h"

/* Shared SSD1306 screen-header style: title on row 0, 1-px signature rule
   on page 1. Used by the launcher, settings, WebDAV, serial-manager, and
   file-picker screens, and by SDK app frames with frame_title_rule set. */

/* 1-px horizontal rule across a display page (bit 3: middle of the 8-px row). */
static inline void jpp_draw_rule(uint8_t page)
{
    ssd1306_fill_page(page, 0x08u);
}

/* 1-px rule on the *bottom* pixel row of a page.  The 5x7 font only reaches
   bit 6, so this shares the row with the text drawn above it instead of
   consuming a whole text line — used for the launcher's system-app divider. */
static inline void jpp_draw_underline_rule(uint8_t page)
{
    ssd1306_or_page(page, 0x80u);
}

/* Title row + the signature rule on page 1. */
static inline void jpp_draw_title(const char *title)
{
    ssd1306_draw_string(0u, 0u, title, false);
    jpp_draw_rule(1u);
}
