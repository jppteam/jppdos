#include "../include/jpp_kbd_core.h"

#include <string.h>
#include <stdio.h>

/* ---- 5×8 ASCII font (codes 0x20–0x7E) ----------------------------------- */
/* Column-major: byte c is column c, bit r (1<<r) is pixel row r (0 = top).
 * Identical to the font in ssd1306.c; kept here so jpp_kbd_core is self-contained
 * without depending on the main/ SSD1306 driver. */
static const uint8_t s_font[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x40,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x0A,0x07,0x00,0x0A,0x07},
};

/* ---- Special-key icon bitmaps (5-col, column-major, bit r = row r, 0=top) */
const uint8_t JPP_KBD_ICON_SHIFT[5]  = { 0x04, 0x06, 0x1F, 0x06, 0x04 };
const uint8_t JPP_KBD_ICON_DONE[5]   = { 0x18, 0x30, 0x18, 0x0C, 0x06 };
const uint8_t JPP_KBD_ICON_CANCEL[5] = { 0x22, 0x14, 0x08, 0x14, 0x22 };
const uint8_t JPP_KBD_ICON_BKSP[5] = { 0x08, 0x1C, 0x2A, 0x08, 0x08 };

/* ---- Pixel primitives ---------------------------------------------------- */

void jpp_kbd_px(jpp_kbd_pixbuf_t buf, int x, int y, bool on)
{
    if (x < 0 || x >= 128 || y < 0 || y >= (int)JPP_KBD_PX_ROWS) { return; }
    uint8_t *byte = &buf[y][x / 8];
    uint8_t  bit  = (uint8_t)(0x80u >> (x % 8));
    if (on) { *byte |= bit; } else { *byte &= (uint8_t)~bit; }
}

void jpp_kbd_glyph(jpp_kbd_pixbuf_t buf, int x, int y, char ch, bool knockout)
{
    unsigned idx = (unsigned)(uint8_t)ch - 0x20u;
    if (idx >= sizeof(s_font) / sizeof(s_font[0])) { return; }
    const uint8_t *g = s_font[idx];
    for (int c = 0; c < 5; c++) {
        for (int r = 0; r < 8; r++) {
            if (g[c] & (uint8_t)(1u << r)) {
                jpp_kbd_px(buf, x + c, y + r, !knockout);
            }
        }
    }
}

void jpp_kbd_text(jpp_kbd_pixbuf_t buf, int x, int y,
                  const char *s, int max_x, bool knockout)
{
    for (size_t i = 0u; s[i] != '\0'; i++) {
        if (x + 5 > max_x) { break; }
        jpp_kbd_glyph(buf, x, y, s[i], knockout);
        x += 6;
    }
}

void jpp_kbd_fill_rect(jpp_kbd_pixbuf_t buf, int x0, int y0, int x1, int y1, bool on)
{
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            jpp_kbd_px(buf, x, y, on);
        }
    }
}

void jpp_kbd_frame_rect(jpp_kbd_pixbuf_t buf, int x0, int y0, int x1, int y1)
{
    for (int x = x0; x <= x1; x++) {
        jpp_kbd_px(buf, x, y0, true);
        jpp_kbd_px(buf, x, y1, true);
    }
    for (int y = y0; y <= y1; y++) {
        jpp_kbd_px(buf, x0, y, true);
        jpp_kbd_px(buf, x1, y, true);
    }
}

void jpp_kbd_icon(jpp_kbd_pixbuf_t buf, int x, int y,
                  const uint8_t bmp[5], bool knockout)
{
    for (int c = 0; c < 5; c++) {
        for (int r = 0; r < 8; r++) {
            if (bmp[c] & (uint8_t)(1u << r)) {
                jpp_kbd_px(buf, x + c, y + r, !knockout);
            }
        }
    }
}

/* ---- Layout -------------------------------------------------------------- */

void jpp_kbd_layout(jpp_kbd_input_type_t type,
                    jpp_kbd_key_t rows[JPP_KBD_MAX_ROWS][JPP_KBD_MAX_COLS],
                    size_t row_len[JPP_KBD_MAX_ROWS],
                    size_t *row_count)
{
    static const char *DIGITS  = "1234567890";
    static const char *SYMBOLS = "!?@#*/:.()";
    size_t i;
    for (i = 0u; DIGITS[i]; i++) {
        rows[0][i] = (jpp_kbd_key_t){DIGITS[i], JPP_KBD_KIND_CHAR, SYMBOLS[i]};
    }
    row_len[0] = 10u;

    if (type == JPP_KBD_TYPE_TEXT) {
        static const char *RA = "qwertyuiop";
        static const char *RB = "asdfghjkl";
        static const char *RC = "zxcvbnm";
        size_t a = 0u;
        for (; RA[a]; a++) { rows[1][a] = (jpp_kbd_key_t){.ch = RA[a], .kind = JPP_KBD_KIND_CHAR}; }
        row_len[1] = a;
        size_t b = 0u;
        for (; RB[b]; b++) { rows[2][b] = (jpp_kbd_key_t){.ch = RB[b], .kind = JPP_KBD_KIND_CHAR}; }
        rows[2][b++] = (jpp_kbd_key_t){.ch = '^', .kind = JPP_KBD_KIND_SHIFT};
        row_len[2] = b;
        size_t c = 0u;
        for (; RC[c]; c++) { rows[3][c] = (jpp_kbd_key_t){.ch = RC[c], .kind = JPP_KBD_KIND_CHAR}; }
        rows[3][c++] = (jpp_kbd_key_t){.ch = '_', .kind = JPP_KBD_KIND_SPACE};
        rows[3][c++] = (jpp_kbd_key_t){.ch = '<', .kind = JPP_KBD_KIND_BKSP};
        rows[3][c++] = (jpp_kbd_key_t){.ch = '*', .kind = JPP_KBD_KIND_DONE};
        rows[3][c++] = (jpp_kbd_key_t){.ch = 'X', .kind = JPP_KBD_KIND_CANCEL};
        row_len[3] = c;
        *row_count = 4u;
    } else {
        size_t b = 0u;
        if (type == JPP_KBD_TYPE_NUMBER) {
            rows[1][b++] = (jpp_kbd_key_t){.ch = '-', .kind = JPP_KBD_KIND_CHAR};
            rows[1][b++] = (jpp_kbd_key_t){.ch = '.', .kind = JPP_KBD_KIND_CHAR};
        } else if (type == JPP_KBD_TYPE_DATE) {
            rows[1][b++] = (jpp_kbd_key_t){.ch = '-', .kind = JPP_KBD_KIND_CHAR};
        } else {
            rows[1][b++] = (jpp_kbd_key_t){.ch = ':', .kind = JPP_KBD_KIND_CHAR};
        }
        rows[1][b++] = (jpp_kbd_key_t){.ch = '<', .kind = JPP_KBD_KIND_BKSP};
        rows[1][b++] = (jpp_kbd_key_t){.ch = '*', .kind = JPP_KBD_KIND_DONE};
        rows[1][b++] = (jpp_kbd_key_t){.ch = 'X', .kind = JPP_KBD_KIND_CANCEL};
        row_len[1] = b;
        *row_count = 2u;
    }
}

/* ---- Cursor -------------------------------------------------------------- */

void jpp_kbd_cursor_init(jpp_kbd_cursor_t *cursor, const char *prefill)
{
    if (cursor == NULL) { return; }
    memset(cursor, 0, sizeof(*cursor));
    if (prefill != NULL && prefill[0] != '\0') {
        size_t n = strlen(prefill);
        if (n > JPP_KBD_VALUE_MAX) { n = JPP_KBD_VALUE_MAX; }
        memcpy(cursor->value, prefill, n);
        cursor->value[n] = '\0';
        cursor->len = n;
    }
}

/* sdk_key values match jpp_sdk_key_event_t in jpp_sdk_bridge.h:
 *  0=NONE 1=UP 2=DOWN 3=LEFT 4=RIGHT 5=OK 6=OK_LONG */
jpp_kbd_result_t jpp_kbd_step(jpp_kbd_cursor_t *cursor,
                               const jpp_kbd_key_t rows[JPP_KBD_MAX_ROWS][JPP_KBD_MAX_COLS],
                               const size_t row_len[JPP_KBD_MAX_ROWS],
                               size_t row_count,
                               int sdk_key,
                               size_t cap)
{
    enum { KEY_NONE=0, KEY_UP=1, KEY_DOWN=2, KEY_LEFT=3,
           KEY_RIGHT=4, KEY_OK=5, KEY_OK_LONG=6 };

    if (sdk_key == KEY_OK_LONG) { return JPP_KBD_RESULT_CANCEL; }

    switch (sdk_key) {
    case KEY_UP:
        cursor->focus_row = (cursor->focus_row == 0u)
                            ? row_count - 1u : cursor->focus_row - 1u;
        break;
    case KEY_DOWN:
        cursor->focus_row = (cursor->focus_row + 1u) % row_count;
        break;
    case KEY_LEFT:
        cursor->focus_col = (cursor->focus_col == 0u)
                            ? row_len[cursor->focus_row] - 1u
                            : cursor->focus_col - 1u;
        break;
    case KEY_RIGHT:
        cursor->focus_col = (cursor->focus_col + 1u) % row_len[cursor->focus_row];
        break;
    case KEY_OK: {
        jpp_kbd_key_t k = rows[cursor->focus_row][cursor->focus_col];
        switch (k.kind) {
        case JPP_KBD_KIND_SHIFT:
            cursor->upper = !cursor->upper;
            break;
        case JPP_KBD_KIND_BKSP:
            if (cursor->len > 0u) { cursor->value[--cursor->len] = '\0'; }
            break;
        case JPP_KBD_KIND_SPACE:
            if (cursor->len < cap) {
                cursor->value[cursor->len++] = ' ';
                cursor->value[cursor->len]   = '\0';
            }
            break;
        case JPP_KBD_KIND_DONE:
            return JPP_KBD_RESULT_DONE;
        case JPP_KBD_KIND_CANCEL:
            return JPP_KBD_RESULT_CANCEL;
        case JPP_KBD_KIND_CHAR:
        default: {
            char ch = k.ch;
            if (cursor->upper) {
                if (ch >= 'a' && ch <= 'z') {
                    ch = (char)(ch - 'a' + 'A');
                } else if (k.shift_ch != '\0') {
                    ch = k.shift_ch;
                }
            }
            if (cursor->len < cap) {
                cursor->value[cursor->len++] = ch;
                cursor->value[cursor->len]   = '\0';
            }
            break;
        }
        }
        break;
    }
    default:
        break;
    }

    /* Clamp column if the new row is shorter. */
    if (cursor->focus_col >= row_len[cursor->focus_row]) {
        cursor->focus_col = row_len[cursor->focus_row] - 1u;
    }

    return JPP_KBD_RESULT_CONTINUE;
}

/* ---- Full render --------------------------------------------------------- */

void jpp_kbd_render(jpp_kbd_pixbuf_t buf,
                    const jpp_kbd_key_t rows[JPP_KBD_MAX_ROWS][JPP_KBD_MAX_COLS],
                    const size_t row_len[JPP_KBD_MAX_ROWS],
                    size_t row_count,
                    const char *input_text,
                    bool input_is_ghost,
                    const jpp_kbd_cursor_t *cursor)
{
    memset(buf, 0, sizeof(jpp_kbd_pixbuf_t));

    /* Input field: outlined rectangle in rows 0-10, text inside. */
    jpp_kbd_frame_rect(buf, 0, 0, 127, 10);
    if (input_text && input_text[0] != '\0') {
        /* Show tail of value so cursor stays visible. */
        const size_t SHOWN = 19u;
        const char *tail = input_text;
        size_t ilen = strlen(input_text);
        if (ilen > SHOWN) { tail = input_text + (ilen - SHOWN); }
        char field[JPP_KBD_VALUE_MAX + 4u];
        if (input_is_ghost) {
            /* Ghost/placeholder: bracketed and no cursor, so it reads as an
               example rather than text the user already typed. */
            snprintf(field, sizeof(field), "[%s]", tail);
        } else {
            snprintf(field, sizeof(field), "%s_", tail);
        }
        jpp_kbd_text(buf, 4, 2, field, 124, false);
    } else if (!input_is_ghost) {
        /* Empty active field: just show cursor */
        jpp_kbd_text(buf, 4, 2, "_", 124, false);
    }

    /* Keyboard rows starting at canvas row 12.  Each row is 9px tall. */
    for (size_t r = 0u; r < row_count; r++) {
        int row_top = 12 + (int)r * 9;
        int cell_w  = 128 / (int)row_len[r];
        for (size_t c = 0u; c < row_len[r]; c++) {
            jpp_kbd_key_t k   = rows[r][c];
            int cell_x        = (int)c * cell_w;
            int gx            = cell_x + (cell_w - 5) / 2;
            bool focused      = (r == cursor->focus_row && c == cursor->focus_col);
            bool active       = focused || (k.kind == JPP_KBD_KIND_SHIFT && cursor->upper);

            if (active) {
                jpp_kbd_fill_rect(buf, cell_x, row_top - 1,
                                  cell_x + cell_w - 2, row_top + 8, true);
            }

            switch (k.kind) {
            case JPP_KBD_KIND_SHIFT:
                jpp_kbd_icon(buf, gx, row_top, JPP_KBD_ICON_SHIFT, active);
                break;
            case JPP_KBD_KIND_DONE:
                jpp_kbd_icon(buf, gx, row_top, JPP_KBD_ICON_DONE, active);
                break;
            case JPP_KBD_KIND_CANCEL:
                jpp_kbd_icon(buf, gx, row_top, JPP_KBD_ICON_CANCEL, active);
                break;
            case JPP_KBD_KIND_BKSP:
                jpp_kbd_icon(buf, gx, row_top, JPP_KBD_ICON_BKSP, active);
                break;
            default: {
                char glyph = k.ch;
                if (k.kind == JPP_KBD_KIND_CHAR && cursor->upper) {
                    if (glyph >= 'a' && glyph <= 'z') {
                        glyph = (char)(glyph - 'a' + 'A');
                    } else if (k.shift_ch != '\0') {
                        glyph = k.shift_ch;
                    }
                }
                jpp_kbd_glyph(buf, gx, row_top, glyph, active);
                break;
            }
            }
        }
    }
}
