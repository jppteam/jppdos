#pragma once

/*
 * jpp_kbd_core — shared on-screen keyboard pixel renderer.
 *
 * Renders the keyboard (text-field + QWERTY grid) into a raw 128×48 pixel
 * buffer (jpp_kbd_pixbuf_t).  The buffer can then be:
 *   - blitted into a jpp_sdk_context_t canvas for SDK apps (via the main loop)
 *   - flushed directly to the SSD1306 for firmware UI (settings, etc.)
 *
 * This module has no external dependencies beyond stdint/stdbool so it can be
 * used from jpp_core and from main/ equally.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Pixel buffer -------------------------------------------------------- */

/* 128 × 48 pixel buffer — row-major, MSB = leftmost pixel.
 * Rows 0-47 correspond to SSD1306 pages 2-7 when flushed to hardware.
 * Identical in layout to the first 48 rows of jpp_sdk_context_t.canvas
 * (the windowed region — the keyboard never renders in fullscreen mode). */
#define JPP_KBD_PX_ROWS  48u
#define JPP_KBD_PX_COLS  16u   /* 128 pixels / 8 bits = 16 bytes per row */
typedef uint8_t jpp_kbd_pixbuf_t[JPP_KBD_PX_ROWS][JPP_KBD_PX_COLS];

/* ---- Key layout ---------------------------------------------------------- */

/* Input mode selects the character set. */
typedef enum {
    JPP_KBD_TYPE_TEXT   = 0,  /* full QWERTY + shift */
    JPP_KBD_TYPE_NUMBER = 1,  /* digits, sign, decimal point */
    JPP_KBD_TYPE_DATE   = 2,  /* digits and '-' */
    JPP_KBD_TYPE_TIME   = 3,  /* digits and ':' */
} jpp_kbd_input_type_t;

/* Key kinds. */
typedef enum {
    JPP_KBD_KIND_CHAR   = 0, /* literal character; letters honour upper flag */
    JPP_KBD_KIND_SHIFT  = 1, /* toggle upper/lower */
    JPP_KBD_KIND_SPACE  = 2, /* insert space */
    JPP_KBD_KIND_BKSP   = 3, /* delete last character */
    JPP_KBD_KIND_DONE   = 4, /* confirm input */
    JPP_KBD_KIND_CANCEL = 5, /* abandon input */
} jpp_kbd_key_kind_t;

typedef struct {
    char              ch;        /* base glyph for CHAR; legend glyph for others */
    jpp_kbd_key_kind_t kind;
    char              shift_ch;  /* shifted variant for CHAR keys; '\0' = none */
} jpp_kbd_key_t;

#define JPP_KBD_MAX_ROWS 4u
#define JPP_KBD_MAX_COLS 12u

/* Build the keyboard layout for `type`.  Fills rows[][]/row_len[]/row_count. */
void jpp_kbd_layout(jpp_kbd_input_type_t type,
                    jpp_kbd_key_t rows[JPP_KBD_MAX_ROWS][JPP_KBD_MAX_COLS],
                    size_t row_len[JPP_KBD_MAX_ROWS],
                    size_t *row_count);

/* ---- Cursor state -------------------------------------------------------- */

#define JPP_KBD_VALUE_MAX 64u

typedef struct {
    char   value[JPP_KBD_VALUE_MAX + 1u];
    size_t len;
    size_t focus_row;
    size_t focus_col;
    bool   upper;
} jpp_kbd_cursor_t;

void jpp_kbd_cursor_init(jpp_kbd_cursor_t *cursor, const char *prefill);

typedef enum {
    JPP_KBD_RESULT_CONTINUE = 0,
    JPP_KBD_RESULT_DONE,    /* user pressed Done key */
    JPP_KBD_RESULT_CANCEL,  /* user pressed Cancel key or BACK */
} jpp_kbd_result_t;

/* Process one key event.  `sdk_key` should be a jpp_sdk_key_event_t value
   cast to int (avoids pulling jpp_sdk_bridge.h into this header). */
jpp_kbd_result_t jpp_kbd_step(jpp_kbd_cursor_t *cursor,
                               const jpp_kbd_key_t rows[JPP_KBD_MAX_ROWS][JPP_KBD_MAX_COLS],
                               const size_t row_len[JPP_KBD_MAX_ROWS],
                               size_t row_count,
                               int sdk_key,
                               size_t cap);

/* ---- Pixel drawing ------------------------------------------------------- */

/* Write or clear one pixel at (x, y) in buf. */
void jpp_kbd_px(jpp_kbd_pixbuf_t buf, int x, int y, bool on);

/* Draw a 5×8 glyph.  `knockout` = draw OFF into a filled (inverted) cell. */
void jpp_kbd_glyph(jpp_kbd_pixbuf_t buf, int x, int y, char ch, bool knockout);

/* Draw a NUL-terminated string. Clips at max_x. */
void jpp_kbd_text(jpp_kbd_pixbuf_t buf, int x, int y,
                  const char *s, int max_x, bool knockout);

/* Fill a solid rectangle. */
void jpp_kbd_fill_rect(jpp_kbd_pixbuf_t buf,
                       int x0, int y0, int x1, int y1, bool on);

/* Draw a rectangle outline. */
void jpp_kbd_frame_rect(jpp_kbd_pixbuf_t buf,
                        int x0, int y0, int x1, int y1);

/* Draw a raw 5-column column-major icon bitmap. */
void jpp_kbd_icon(jpp_kbd_pixbuf_t buf, int x, int y,
                  const uint8_t bmp[5], bool knockout);

/* ---- Full keyboard render ------------------------------------------------ */

/*
 * Render the keyboard into `buf` (input field + grid).
 * Caller provides all layout + cursor state.
 * The title row is NOT rendered here — caller draws it (page 0) separately.
 */
void jpp_kbd_render(jpp_kbd_pixbuf_t buf,
                    const jpp_kbd_key_t rows[JPP_KBD_MAX_ROWS][JPP_KBD_MAX_COLS],
                    const size_t row_len[JPP_KBD_MAX_ROWS],
                    size_t row_count,
                    const char *input_text,
                    bool   input_is_ghost,
                    const jpp_kbd_cursor_t *cursor);

/* ---- Exported icon bitmaps (needed by SDK bridge too) ------------------- */
extern const uint8_t JPP_KBD_ICON_SHIFT[5];
extern const uint8_t JPP_KBD_ICON_DONE[5];
extern const uint8_t JPP_KBD_ICON_CANCEL[5];

#ifdef __cplusplus
}
#endif
