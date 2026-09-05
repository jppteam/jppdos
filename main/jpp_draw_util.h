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

/* Horizontal inset of the divider rule: one character cell per side.  Run
   edge to edge it reads as a display artifact rather than a divider, and
   inset by exactly one cell it lines up with the list text (which starts at
   column 1, after the cursor character). */
#define JPP_DRAW_DIVIDER_INSET SSD1306_CHAR_W

/* In-list separator rule, centred in a page of its own (bit 3), so it has 4 px
   of clear space above and below.  It cannot share a text row: the 5x7 font
   fills bits 0-6, leaving a single spare pixel row, and a rule sitting in it
   touches the glyphs on both sides.  Used for the launcher's system/SD
   divider, whose row the shell leaves blank for exactly this. */
static inline void jpp_draw_divider_rule(uint8_t page)
{
    ssd1306_or_cols(page, JPP_DRAW_DIVIDER_INSET,
                    (uint8_t)(SSD1306_WIDTH - 2u * JPP_DRAW_DIVIDER_INSET),
                    0x08u);
}

/* Title row + the signature rule on page 1. */
static inline void jpp_draw_title(const char *title)
{
    ssd1306_draw_string(0u, 0u, title, false);
    jpp_draw_rule(1u);
}
