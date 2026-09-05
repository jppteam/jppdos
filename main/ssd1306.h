#pragma once

/*
 * Minimal SSD1306 128×64 I²C driver for ESP-IDF v5 (i2c_master API).
 *
 * All rendering goes into an in-memory framebuffer; call ssd1306_flush()
 * to push it to the display over I²C.
 *
 * Coordinate system
 * -----------------
 *   col  : 0 (left) … 127 (right)
 *   page : 0 (top)  …   7 (bottom)  [each page = 8 pixel rows]
 *
 * Character sizes
 *   1× : 5 data cols + 1 gap = 6 px wide, 1 page (8 px) tall
 *   2× : 10 data cols + 2 gap = 12 px wide, 2 pages (16 px) tall
 */

#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSD1306_CHAR_W      6u   /* char width at 1× (incl. gap) */
#define SSD1306_CHAR_W_2X  12u  /* char width at 2× (incl. gap) */
#define SSD1306_PAGES       8u
#define SSD1306_WIDTH     128u

/**
 * Initialise the SSD1306 on an already-created I²C master bus.
 * Returns true on success; the display is cleared and switched on.
 */
bool ssd1306_init(i2c_master_bus_handle_t bus, uint8_t i2c_addr);

/** Zero the framebuffer (all pixels off). */
void ssd1306_clear(void);

/** Fill an entire 8-px-tall page with a raw byte pattern (0xFF = all on). */
void ssd1306_fill_page(uint8_t page, uint8_t pattern);

/**
 * OR a raw byte pattern into a horizontal span of an 8-px-tall page, leaving
 * the pixels already there untouched.  Used to lay a 1-px rule into the spare
 * bottom bit of a text row (the 5x7 font only reaches bit 6).  The span is
 * clipped to the display width.
 */
void ssd1306_or_cols(uint8_t page, uint8_t col, uint8_t width, uint8_t pattern);

/**
 * Draw a single character at (page, col).
 * invert=true swaps on/off pixels (white bg, black text).
 * Returns the column after the character (i.e. col + SSD1306_CHAR_W).
 */
uint8_t ssd1306_draw_char(uint8_t page, uint8_t col, char c, bool invert);

/**
 * Draw a NUL-terminated string at (page, col).
 * Returns the column after the last character.
 */
uint8_t ssd1306_draw_string(uint8_t page, uint8_t col,
                             const char *str, bool invert);

/**
 * Draw a character at 2× scale (2 pages tall, double width).
 * page and page+1 are both written.
 */
uint8_t ssd1306_draw_char_2x(uint8_t page, uint8_t col, char c, bool invert);

/** Draw a string at 2× scale. */
uint8_t ssd1306_draw_string_2x(uint8_t page, uint8_t col,
                                const char *str, bool invert);

/**
 * Draw an 8×8 monochrome bitmap at (page, col).
 * bitmap[0] is the top row; each byte is 8 pixels, MSB = leftmost pixel.
 * invert=true swaps on/off pixels.
 */
void ssd1306_draw_bitmap_8x8(uint8_t page, uint8_t col,
                              const uint8_t *bitmap, bool invert);

/**
 * Draw a 32×32 monochrome bitmap at (page, col).
 * Spans 4 pages (page .. page+3) and 32 columns (col .. col+31).
 * bitmap: 32 rows × 4 bytes/row = 128 bytes, row-major, MSB = leftmost pixel.
 * invert=true swaps on/off pixels.
 */
void ssd1306_draw_bitmap_32x32(uint8_t page, uint8_t col,
                                const uint8_t *bitmap, bool invert);

/** Push the framebuffer to the display over I²C. */
void ssd1306_flush(void);

/** Set display contrast (0x00 = off, 0xFF = max; default after init = 0xCF). */
void ssd1306_set_contrast(uint8_t value);

/** Turn the display panel on or off without changing the framebuffer. */
void ssd1306_display_on(bool on);

/** Set pre-charge (0xD9) and VCOMH deselect (0xDB) registers.
 *  Normal: 0xF1 / 0x40.  Maximum dim: 0x00 / 0x00. */
void ssd1306_set_power_registers(uint8_t precharge, uint8_t vcomh);

#ifdef __cplusplus
}
#endif
