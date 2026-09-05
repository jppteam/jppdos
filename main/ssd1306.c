/*
 * ssd1306.c — SSD1306 128×64 I²C display driver (ESP-IDF v5 i2c_master API)
 *
 * Rendering model
 * ---------------
 * All drawing operations write into a 1 024-byte framebuffer
 * (8 pages × 128 columns, 1 bit per pixel).  ssd1306_flush() sends the
 * whole buffer to the chip in horizontal-addressing mode so a single
 * contiguous DMA/I²C transfer covers the entire screen.
 *
 * Font
 * ----
 * 5×8 (5 data columns, 8 pixel rows).  Each column byte has bit-0 at the
 * top and bit-7 at the bottom, matching the SSD1306 page orientation.
 * A 1-column gap is appended at 1× (→ 6 px per char); 2 columns at 2×
 * (→ 12 px per char).
 */

#include "ssd1306.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "ssd1306";

/* ---- I²C helpers -------------------------------------------------------- */

#define SSD1306_CTRL_CMD  0x00u  /* Co=0, D/C#=0 — stream of commands */
#define SSD1306_CTRL_DATA 0x40u  /* Co=0, D/C#=1 — stream of data     */

static i2c_master_dev_handle_t s_dev = NULL;

static void cmd1(uint8_t c)
{
    uint8_t buf[2] = { SSD1306_CTRL_CMD, c };
    i2c_master_transmit(s_dev, buf, 2, 50);
}

static void cmd2(uint8_t c, uint8_t a)
{
    uint8_t buf[3] = { SSD1306_CTRL_CMD, c, a };
    i2c_master_transmit(s_dev, buf, 3, 50);
}

/* ---- Framebuffer -------------------------------------------------------- */

static uint8_t s_fb[SSD1306_PAGES][SSD1306_WIDTH];

/* ---- 5×8 ASCII font (codes 32 … 126) ----------------------------------- */

static const uint8_t s_font[][5] = {
    /* 0x20 */  {0x00,0x00,0x00,0x00,0x00}, /* ' '  */
    /* 0x21 */  {0x00,0x00,0x5F,0x00,0x00}, /* '!'  */
    /* 0x22 */  {0x00,0x07,0x00,0x07,0x00}, /* '"'  */
    /* 0x23 */  {0x14,0x7F,0x14,0x7F,0x14}, /* '#'  */
    /* 0x24 */  {0x24,0x2A,0x7F,0x2A,0x12}, /* '$'  */
    /* 0x25 */  {0x23,0x13,0x08,0x64,0x62}, /* '%'  */
    /* 0x26 */  {0x36,0x49,0x55,0x22,0x50}, /* '&'  */
    /* 0x27 */  {0x00,0x05,0x03,0x00,0x00}, /* '\'' */
    /* 0x28 */  {0x00,0x1C,0x22,0x41,0x00}, /* '('  */
    /* 0x29 */  {0x00,0x41,0x22,0x1C,0x00}, /* ')'  */
    /* 0x2A */  {0x14,0x08,0x3E,0x08,0x14}, /* '*'  */
    /* 0x2B */  {0x08,0x08,0x3E,0x08,0x08}, /* '+'  */
    /* 0x2C */  {0x00,0x50,0x30,0x00,0x00}, /* ','  */
    /* 0x2D */  {0x08,0x08,0x08,0x08,0x08}, /* '-'  */
    /* 0x2E */  {0x00,0x60,0x60,0x00,0x00}, /* '.'  */
    /* 0x2F */  {0x20,0x10,0x08,0x04,0x02}, /* '/'  */
    /* 0x30 */  {0x3E,0x51,0x49,0x45,0x3E}, /* '0'  */
    /* 0x31 */  {0x00,0x42,0x7F,0x40,0x00}, /* '1'  */
    /* 0x32 */  {0x42,0x61,0x51,0x49,0x46}, /* '2'  */
    /* 0x33 */  {0x21,0x41,0x45,0x4B,0x31}, /* '3'  */
    /* 0x34 */  {0x18,0x14,0x12,0x7F,0x10}, /* '4'  */
    /* 0x35 */  {0x27,0x45,0x45,0x45,0x39}, /* '5'  */
    /* 0x36 */  {0x3C,0x4A,0x49,0x49,0x30}, /* '6'  */
    /* 0x37 */  {0x01,0x71,0x09,0x05,0x03}, /* '7'  */
    /* 0x38 */  {0x36,0x49,0x49,0x49,0x36}, /* '8'  */
    /* 0x39 */  {0x06,0x49,0x49,0x29,0x1E}, /* '9'  */
    /* 0x3A */  {0x00,0x36,0x36,0x00,0x00}, /* ':'  */
    /* 0x3B */  {0x00,0x56,0x36,0x00,0x00}, /* ';'  */
    /* 0x3C */  {0x08,0x14,0x22,0x41,0x00}, /* '<'  */
    /* 0x3D */  {0x14,0x14,0x14,0x14,0x14}, /* '='  */
    /* 0x3E */  {0x00,0x41,0x22,0x14,0x08}, /* '>'  */
    /* 0x3F */  {0x02,0x01,0x51,0x09,0x06}, /* '?'  */
    /* 0x40 */  {0x32,0x49,0x79,0x41,0x3E}, /* '@'  */
    /* 0x41 */  {0x7E,0x11,0x11,0x11,0x7E}, /* 'A'  */
    /* 0x42 */  {0x7F,0x49,0x49,0x49,0x36}, /* 'B'  */
    /* 0x43 */  {0x3E,0x41,0x41,0x41,0x22}, /* 'C'  */
    /* 0x44 */  {0x7F,0x41,0x41,0x22,0x1C}, /* 'D'  */
    /* 0x45 */  {0x7F,0x49,0x49,0x49,0x41}, /* 'E'  */
    /* 0x46 */  {0x7F,0x09,0x09,0x09,0x01}, /* 'F'  */
    /* 0x47 */  {0x3E,0x41,0x49,0x49,0x7A}, /* 'G'  */
    /* 0x48 */  {0x7F,0x08,0x08,0x08,0x7F}, /* 'H'  */
    /* 0x49 */  {0x00,0x41,0x7F,0x41,0x00}, /* 'I'  */
    /* 0x4A */  {0x20,0x40,0x41,0x3F,0x01}, /* 'J'  */
    /* 0x4B */  {0x7F,0x08,0x14,0x22,0x41}, /* 'K'  */
    /* 0x4C */  {0x7F,0x40,0x40,0x40,0x40}, /* 'L'  */
    /* 0x4D */  {0x7F,0x02,0x0C,0x02,0x7F}, /* 'M'  */
    /* 0x4E */  {0x7F,0x04,0x08,0x10,0x7F}, /* 'N'  */
    /* 0x4F */  {0x3E,0x41,0x41,0x41,0x3E}, /* 'O'  */
    /* 0x50 */  {0x7F,0x09,0x09,0x09,0x06}, /* 'P'  */
    /* 0x51 */  {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q'  */
    /* 0x52 */  {0x7F,0x09,0x19,0x29,0x46}, /* 'R'  */
    /* 0x53 */  {0x46,0x49,0x49,0x49,0x31}, /* 'S'  */
    /* 0x54 */  {0x01,0x01,0x7F,0x01,0x01}, /* 'T'  */
    /* 0x55 */  {0x3F,0x40,0x40,0x40,0x3F}, /* 'U'  */
    /* 0x56 */  {0x1F,0x20,0x40,0x20,0x1F}, /* 'V'  */
    /* 0x57 */  {0x3F,0x40,0x38,0x40,0x3F}, /* 'W'  */
    /* 0x58 */  {0x63,0x14,0x08,0x14,0x63}, /* 'X'  */
    /* 0x59 */  {0x07,0x08,0x70,0x08,0x07}, /* 'Y'  */
    /* 0x5A */  {0x61,0x51,0x49,0x45,0x43}, /* 'Z'  */
    /* 0x5B */  {0x00,0x7F,0x41,0x41,0x00}, /* '['  */
    /* 0x5C */  {0x02,0x04,0x08,0x10,0x20}, /* '\\' */
    /* 0x5D */  {0x00,0x41,0x41,0x7F,0x00}, /* ']'  */
    /* 0x5E */  {0x04,0x02,0x01,0x02,0x04}, /* '^'  */
    /* 0x5F */  {0x40,0x40,0x40,0x40,0x40}, /* '_'  */
    /* 0x60 */  {0x00,0x01,0x02,0x04,0x00}, /* '`'  */
    /* 0x61 */  {0x20,0x54,0x54,0x54,0x78}, /* 'a'  */
    /* 0x62 */  {0x7F,0x48,0x44,0x44,0x38}, /* 'b'  */
    /* 0x63 */  {0x38,0x44,0x44,0x44,0x20}, /* 'c'  */
    /* 0x64 */  {0x38,0x44,0x44,0x48,0x7F}, /* 'd'  */
    /* 0x65 */  {0x38,0x54,0x54,0x54,0x18}, /* 'e'  */
    /* 0x66 */  {0x08,0x7E,0x09,0x01,0x02}, /* 'f'  */
    /* 0x67 */  {0x0C,0x52,0x52,0x52,0x3E}, /* 'g'  */
    /* 0x68 */  {0x7F,0x08,0x04,0x04,0x78}, /* 'h'  */
    /* 0x69 */  {0x00,0x44,0x7D,0x40,0x00}, /* 'i'  */
    /* 0x6A */  {0x20,0x40,0x44,0x3D,0x00}, /* 'j'  */
    /* 0x6B */  {0x7F,0x10,0x28,0x44,0x00}, /* 'k'  */
    /* 0x6C */  {0x00,0x41,0x7F,0x40,0x00}, /* 'l'  */
    /* 0x6D */  {0x7C,0x04,0x18,0x04,0x78}, /* 'm'  */
    /* 0x6E */  {0x7C,0x08,0x04,0x04,0x78}, /* 'n'  */
    /* 0x6F */  {0x38,0x44,0x44,0x44,0x38}, /* 'o'  */
    /* 0x70 */  {0x7C,0x14,0x14,0x14,0x08}, /* 'p'  */
    /* 0x71 */  {0x08,0x14,0x14,0x18,0x7C}, /* 'q'  */
    /* 0x72 */  {0x7C,0x08,0x04,0x04,0x08}, /* 'r'  */
    /* 0x73 */  {0x48,0x54,0x54,0x54,0x20}, /* 's'  */
    /* 0x74 */  {0x04,0x3F,0x44,0x40,0x20}, /* 't'  */
    /* 0x75 */  {0x3C,0x40,0x40,0x40,0x7C}, /* 'u'  */
    /* 0x76 */  {0x1C,0x20,0x40,0x20,0x1C}, /* 'v'  */
    /* 0x77 */  {0x3C,0x40,0x30,0x40,0x3C}, /* 'w'  */
    /* 0x78 */  {0x44,0x28,0x10,0x28,0x44}, /* 'x'  */
    /* 0x79 */  {0x0C,0x50,0x50,0x50,0x3C}, /* 'y'  */
    /* 0x7A */  {0x44,0x64,0x54,0x4C,0x44}, /* 'z'  */
    /* 0x7B */  {0x00,0x08,0x36,0x41,0x00}, /* '{'  */
    /* 0x7C */  {0x00,0x00,0x7F,0x00,0x00}, /* '|'  */
    /* 0x7D */  {0x00,0x41,0x36,0x08,0x00}, /* '}'  */
    /* 0x7E */  {0x0A,0x07,0x00,0x0A,0x07}, /* '~'  */
    /* Past ASCII: d-pad arrows for on-screen control hints.  See the
       SSD1306_ARROW_* macros in the header — nothing else draws bytes
       above 0x7E, so these slots cannot collide with real text. */
    /* 0x7F */  {0x08,0x1C,0x2A,0x08,0x08}, /* left arrow  */
    /* 0x80 */  {0x08,0x08,0x2A,0x1C,0x08}, /* right arrow */
};

static const uint8_t *glyph(char c)
{
    static const uint8_t blank[5] = {0,0,0,0,0};
    unsigned idx = (unsigned)(uint8_t)c - 0x20u;
    if (idx >= sizeof(s_font) / sizeof(s_font[0])) {
        return blank;
    }
    return s_font[idx];
}

/* ---- Low-level pixel helpers -------------------------------------------- */

static void put_col(uint8_t page, uint8_t col, uint8_t val, bool invert)
{
    if (page >= SSD1306_PAGES || col >= SSD1306_WIDTH) {
        return;
    }
    s_fb[page][col] = invert ? (uint8_t)(~val) : val;
}

/* ---- Public API ---------------------------------------------------------- */

bool ssd1306_init(i2c_master_bus_handle_t bus, uint8_t i2c_addr)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = i2c_addr,
        /* 100 kHz: shared I2C bus is capped by the DS1307 RTC (see jpp_hw_config.h). */
        .scl_speed_hz    = 100000u,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(err));
        s_dev = NULL;
        return false;
    }

    /* SSD1306 initialisation sequence (128×64) */
    cmd1(0xAE);           /* display off                   */
    cmd2(0xD5, 0x80);     /* clock div / osc freq          */
    cmd2(0xA8, 0x3F);     /* multiplex: 64 rows            */
    cmd2(0xD3, 0x00);     /* display offset: 0             */
    cmd1(0x40);           /* start line: 0                 */
    cmd2(0x8D, 0x14);     /* charge pump: enable           */
    cmd2(0x20, 0x00);     /* memory addr mode: horizontal  */
    cmd1(0xA1);           /* seg remap (col 127 = SEG0)    */
    cmd1(0xC8);           /* COM scan: decrement           */
    cmd2(0xDA, 0x12);     /* COM pins: alt, no remap       */
    cmd2(0x81, 0xCF);     /* contrast                      */
    cmd2(0xD9, 0xF1);     /* pre-charge period             */
    cmd2(0xDB, 0x40);     /* VCOMH deselect level          */
    cmd1(0xA4);           /* display follows RAM           */
    cmd1(0xA6);           /* normal (non-inverted) display */
    cmd1(0xAF);           /* display on                    */

    ssd1306_clear();
    ssd1306_flush();
    return true;
}

void ssd1306_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void ssd1306_fill_page(uint8_t page, uint8_t pattern)
{
    if (page >= SSD1306_PAGES) {
        return;
    }
    memset(s_fb[page], pattern, SSD1306_WIDTH);
}

void ssd1306_or_cols(uint8_t page, uint8_t col, uint8_t width, uint8_t pattern)
{
    if (page >= SSD1306_PAGES || col >= SSD1306_WIDTH) {
        return;
    }
    uint16_t end = (uint16_t)col + (uint16_t)width;
    if (end > SSD1306_WIDTH) {
        end = SSD1306_WIDTH;
    }
    for (uint16_t c = col; c < end; c++) {
        s_fb[page][c] |= pattern;
    }
}

uint8_t ssd1306_draw_char(uint8_t page, uint8_t col, char c, bool invert)
{
    const uint8_t *g = glyph(c);
    for (uint8_t i = 0; i < 5u; i++) {
        put_col(page, (uint8_t)(col + i), g[i], invert);
    }
    put_col(page, (uint8_t)(col + 5u), 0x00u, invert); /* gap */
    return (uint8_t)(col + (uint8_t)SSD1306_CHAR_W);
}

uint8_t ssd1306_draw_string(uint8_t page, uint8_t col,
                             const char *str, bool invert)
{
    while (*str != '\0' && col + SSD1306_CHAR_W <= SSD1306_WIDTH) {
        col = ssd1306_draw_char(page, col, *str++, invert);
    }
    return col;
}

/*
 * expand4 — replicate each of the 4 low bits to a pair of adjacent bits.
 *
 * Used to double the vertical resolution of a glyph column:
 *   bit-0 → bits 1:0
 *   bit-1 → bits 3:2
 *   bit-2 → bits 5:4
 *   bit-3 → bits 7:6
 */
static uint8_t expand4(uint8_t nibble)
{
    uint8_t out = 0u;
    for (int i = 0; i < 4; i++) {
        if (nibble & (uint8_t)(1u << i)) {
            out |= (uint8_t)(3u << (i * 2));
        }
    }
    return out;
}

uint8_t ssd1306_draw_char_2x(uint8_t page, uint8_t col, char c, bool invert)
{
    if (page + 1u >= SSD1306_PAGES) {
        /* Not enough pages for 2×; fall back to 1×. */
        return ssd1306_draw_char(page, col, c, invert);
    }

    const uint8_t *g = glyph(c);

    for (uint8_t i = 0u; i < 5u; i++) {
        uint8_t byte  = g[i];
        /* Low nibble (bits 3:0) → top page; high nibble → bottom page */
        uint8_t upper = expand4(byte & 0x0Fu);
        uint8_t lower = expand4((byte >> 4u) & 0x0Fu);

        uint8_t c0 = (uint8_t)(col + i * 2u);
        uint8_t c1 = (uint8_t)(c0 + 1u);

        put_col(page,      c0, upper, invert);
        put_col(page,      c1, upper, invert);
        put_col(page + 1u, c0, lower, invert);
        put_col(page + 1u, c1, lower, invert);
    }

    /* Two-column gap */
    uint8_t g0 = (uint8_t)(col + 10u);
    uint8_t g1 = (uint8_t)(g0 + 1u);
    put_col(page,      g0, 0x00u, invert);
    put_col(page,      g1, 0x00u, invert);
    put_col(page + 1u, g0, 0x00u, invert);
    put_col(page + 1u, g1, 0x00u, invert);

    return (uint8_t)(col + (uint8_t)SSD1306_CHAR_W_2X);
}

uint8_t ssd1306_draw_string_2x(uint8_t page, uint8_t col,
                                const char *str, bool invert)
{
    while (*str != '\0' && col + SSD1306_CHAR_W_2X <= SSD1306_WIDTH) {
        col = ssd1306_draw_char_2x(page, col, *str++, invert);
    }
    return col;
}

void ssd1306_draw_bitmap_8x8(uint8_t page, uint8_t col,
                              const uint8_t *bitmap, bool invert)
{
    uint8_t row;

    if (bitmap == NULL || page >= SSD1306_PAGES) {
        return;
    }
    /*
     * Each byte in `bitmap` is one pixel row (MSB = leftmost pixel).
     * The SSD1306 framebuffer stores columns as vertical bit-strips
     * (bit-0 = top of page).  We therefore need to transpose: for each
     * column c (0-7), collect bit c from every row byte and pack them
     * into a single framebuffer column byte.
     */
    for (uint8_t c = 0u; c < 8u; c++) {
        uint8_t col_byte = 0u;
        for (row = 0u; row < 8u; row++) {
            if (bitmap[row] & (uint8_t)(0x80u >> c)) {
                col_byte |= (uint8_t)(1u << row);
            }
        }
        put_col(page, (uint8_t)(col + c), col_byte, invert);
    }
}

void ssd1306_draw_bitmap_32x32(uint8_t page, uint8_t col,
                                const uint8_t *bitmap, bool invert)
{
    if (bitmap == NULL || page + 3u >= SSD1306_PAGES) {
        return;
    }
    for (uint8_t p = 0u; p < 4u; p++) {
        for (uint8_t c = 0u; c < 32u; c++) {
            uint8_t col_byte = 0u;
            uint8_t byte_idx = c / 8u;
            uint8_t bit_mask = (uint8_t)(0x80u >> (c % 8u));
            for (uint8_t r = 0u; r < 8u; r++) {
                if (bitmap[(p * 8u + r) * 4u + byte_idx] & bit_mask) {
                    col_byte |= (uint8_t)(1u << r);
                }
            }
            put_col((uint8_t)(page + p), (uint8_t)(col + c), col_byte, invert);
        }
    }
}

void ssd1306_flush(void)
{
    if (s_dev == NULL) {
        return;
    }

    /* Set horizontal addressing window: all columns, all pages. */
    cmd1(0x21); cmd1(0x00); cmd1(0x7F); /* column 0 … 127 */
    cmd1(0x22); cmd1(0x00); cmd1(0x07); /* page   0 … 7   */

    /*
     * Stream 128 data bytes per page.  Each I²C packet is:
     *   [control byte 0x40] [128 pixel bytes]
     * = 129 bytes — well within the I²C master driver limits.
     */
    uint8_t buf[SSD1306_WIDTH + 1u];
    buf[0] = SSD1306_CTRL_DATA;
    for (uint8_t page = 0u; page < SSD1306_PAGES; page++) {
        memcpy(buf + 1u, s_fb[page], SSD1306_WIDTH);
        i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
    }
}

void ssd1306_set_contrast(uint8_t value)
{
    if (s_dev == NULL) { return; }
    cmd2(0x81, value);
}

void ssd1306_display_on(bool on)
{
    if (s_dev == NULL) { return; }
    cmd1(on ? 0xAFu : 0xAEu);
}

void ssd1306_set_power_registers(uint8_t precharge, uint8_t vcomh)
{
    if (s_dev == NULL) { return; }
    cmd2(0xD9, precharge);
    cmd2(0xDB, vcomh);
}
