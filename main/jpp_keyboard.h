#pragma once

/*
 * jpp_keyboard — on-screen keyboard for firmware UI (settings).
 *
 * Renders the shared jpp_kbd_core keyboard directly to the SSD1306 and reads
 * key events from s_action_queue.  Blocks until the user confirms or cancels.
 * Safe to call from within the main-loop action-handler path.
 */

#include <stdbool.h>
#include <stddef.h>
#include "jpp_kbd_core.h"
#include "jpp_ui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Show the on-screen keyboard and collect text input.
 *   title    — prompt shown at the top (SSD1306 page 0)
 *   prefill  — initial value; NULL or "" for empty
 *   type     — character set (text / number / date / time)
 *   out      — output buffer (NUL-terminated on return)
 *   out_len  — size of out including the NUL terminator
 *
 * Returns true if the user confirmed (pressed Done), false if cancelled.
 */
bool jpp_keyboard_input(const char *title,
                        const char *prefill,
                        jpp_kbd_input_type_t type,
                        jpp_ui_shell_t *shell,
                        char *out,
                        size_t out_len);

#ifdef __cplusplus
}
#endif
