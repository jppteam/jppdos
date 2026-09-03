#include "jpp_keyboard.h"
#include "ssd1306.h"
#include "jpp_app_dispatch.h"   /* for s_action_queue (extern QueueHandle_t) */
#include "jpp_ui_core.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* ---- SSD1306 canvas flush ------------------------------------------------ */

/*
 * Flush a jpp_kbd_pixbuf_t (128×48 pixels) to SSD1306 pages 2-7.
 * The 48 pixel rows map to 6 pages; each page holds 8 pixel rows.
 * We blit 8-column blocks via ssd1306_draw_bitmap_8x8 (page, col, bitmap).
 * The bitmap layout (row[] = pixel-row byte, MSB=left) matches the pixbuf.
 */
static void flush_pixbuf_to_ssd1306(const jpp_kbd_pixbuf_t buf)
{
    for (uint8_t page = 0u; page < 6u; page++) {        /* pages 2-7 */
        for (uint8_t blk = 0u; blk < 16u; blk++) {     /* 16 × 8-col blocks */
            uint8_t bitmap[8];
            for (uint8_t r = 0u; r < 8u; r++) {
                bitmap[r] = buf[(size_t)(page * 8u + r)][blk];
            }
            ssd1306_draw_bitmap_8x8((uint8_t)(page + 2u), (uint8_t)(blk * 8u),
                                    bitmap, false);
        }
    }
}

/* ---- Map jpp_ui_action_t → sdk_key integer used by jpp_kbd_step --------- */
/* jpp_kbd_step expects jpp_sdk_key_event_t values:
 *  0=NONE 1=UP 2=DOWN 3=LEFT 4=RIGHT 5=OK 6=OK_LONG            */
static int action_to_sdk_key(jpp_ui_action_t action)
{
    switch (action) {
    case JPP_UI_ACTION_UP:    return 1;
    case JPP_UI_ACTION_DOWN:  return 2;
    case JPP_UI_ACTION_LEFT:  return 3;
    case JPP_UI_ACTION_RIGHT: return 4;
    case JPP_UI_ACTION_OK:    return 5;
    case JPP_UI_ACTION_BACK:  return 6; /* treat as OK_LONG = cancel */
    default:                  return 0;
    }
}

/* ---- Public API ---------------------------------------------------------- */

bool jpp_keyboard_input(const char *title,
                        const char *prefill,
                        jpp_kbd_input_type_t type,
                        jpp_ui_shell_t *shell,
                        char *out,
                        size_t out_len)
{
    if (out == NULL || out_len == 0u) { return false; }
    out[0] = '\0';

    jpp_kbd_key_t rows[JPP_KBD_MAX_ROWS][JPP_KBD_MAX_COLS];
    size_t row_len[JPP_KBD_MAX_ROWS];
    size_t row_count = 0u;
    jpp_kbd_layout(type, rows, row_len, &row_count);

    jpp_kbd_cursor_t cursor;
    jpp_kbd_cursor_init(&cursor, prefill);
    size_t cap = (out_len - 1u < JPP_KBD_VALUE_MAX) ? out_len - 1u : JPP_KBD_VALUE_MAX;

    jpp_kbd_pixbuf_t pixbuf;

    for (;;) {
        /* Determine what to show in the input field. */
        bool is_ghost = false;
        const char *field_text = cursor.value;
        if (cursor.len == 0u) {
            /* No input yet: show prefill as ghost if available. */
            if (prefill != NULL && prefill[0] != '\0') {
                field_text = prefill;
                is_ghost   = true;
            }
        }

        /* Render keyboard into the pixel buffer. */
        jpp_kbd_render(pixbuf, rows, row_len, row_count,
                       field_text, is_ghost, &cursor);

        /* Flush to display: title on page 0, keyboard pixbuf on pages 2-7. */
        ssd1306_clear();
        if (title != NULL && title[0] != '\0') {
            ssd1306_draw_string(0u, 0u, title, false);
        }
        flush_pixbuf_to_ssd1306(pixbuf);
        ssd1306_flush();

        /* Wait for next key. */
        jpp_ui_action_t action;
        xQueueReceive(s_action_queue, &action, portMAX_DELAY);
        if (shell != NULL) { jpp_ui_shell_note_activity(shell); }

        jpp_kbd_result_t res = jpp_kbd_step(&cursor, rows, row_len, row_count,
                                             action_to_sdk_key(action), cap);
        if (res == JPP_KBD_RESULT_DONE) {
            size_t n = cursor.len;
            if (n >= out_len) { n = out_len - 1u; }
            memcpy(out, cursor.value, n);
            out[n] = '\0';
            return true;
        }
        if (res == JPP_KBD_RESULT_CANCEL) {
            out[0] = '\0';
            return false;
        }
    }
}
