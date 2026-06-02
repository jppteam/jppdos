#pragma once

/*
 * jpp_file_picker — on-screen file browser for firmware UI.
 *
 * Renders a navigable directory listing directly to the SSD1306 and reads
 * key events from s_action_queue. Blocks until the user selects a file or
 * cancels. Long entries scroll as marquees (all visible overflowing items
 * scroll at the same cadence).
 *
 * Analogous to jpp_keyboard for text input; see also jpp_sdk_file_pick()
 * in jpp_sdk_bridge.h for the App SDK counterpart.
 */

#include <stdbool.h>
#include <stddef.h>
#include "jpp_ui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Show a file browser rooted at root_path (typically "/sd").
 *   shell    — used for activity tracking; may be NULL
 *   out_path — receives the absolute path of the selected file (NUL-terminated)
 *   out_len  — size of out_path buffer
 *
 * Returns true if the user selected a file.
 * Returns false if cancelled (BACK pressed at the root directory).
 */
bool jpp_file_picker(const char    *root_path,
                     jpp_ui_shell_t *shell,
                     char           *out_path,
                     size_t          out_len);

#ifdef __cplusplus
}
#endif
