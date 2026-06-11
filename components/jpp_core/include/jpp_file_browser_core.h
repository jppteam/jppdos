#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_FB_ENTRY_MAX    32u  /* max entries per directory (excess dropped) */
#define JPP_FB_NAME_MAX     65u  /* entry name incl. "/" suffix and NUL        */
#define JPP_FB_PATH_MAX    256u  /* absolute path buffer                       */
#define JPP_FB_VISIBLE       6u  /* list rows below the title                  */
#define JPP_FB_ITEM_CHARS   20u  /* name chars after the cursor prefix         */
#define JPP_FB_ROW_MAX      22u  /* cursor char + JPP_FB_ITEM_CHARS + NUL      */
#define JPP_FB_MARQUEE_MS  400u  /* render cadence while idle                  */

typedef enum {
    JPP_FB_KEY_NONE = 0,
    JPP_FB_KEY_UP,
    JPP_FB_KEY_DOWN,
    JPP_FB_KEY_OK,
    JPP_FB_KEY_BACK,
} jpp_fb_key_t;

/* Fill names[i] (entry name, "/"-suffixed for directories) and is_dir[i]
   for abs_dir; return the number of entries written (at most max). */
typedef size_t (*jpp_fb_list_dir_fn)(
    void *ctx,
    const char *abs_dir,
    char names[][JPP_FB_NAME_MAX],
    bool *is_dir,
    size_t max
);

/* Draw the browser screen: a "Select a file:" title (supplied by the shim)
   plus row_count list rows, each already prefixed with '>' or ' '. */
typedef void (*jpp_fb_render_fn)(void *ctx, const char *const *rows, size_t row_count);

/* Block up to timeout_ms for a key; return JPP_FB_KEY_NONE on timeout. */
typedef jpp_fb_key_t (*jpp_fb_wait_key_fn)(void *ctx, uint32_t timeout_ms);

typedef struct {
    jpp_fb_list_dir_fn list_dir;
    jpp_fb_render_fn   render;
    jpp_fb_wait_key_fn wait_key;
    void              *ctx;
} jpp_file_browser_io_t;

/* Run a blocking file browser rooted at root_path: UP/DOWN move, OK enters
   directories or selects a file, BACK goes up a level (cancels at the root);
   a ".." row appears above entries whenever the browser is below the root.
   Returns true with the selected file's absolute path in out_path, false if
   the user cancelled. Browser state lives in one static buffer, so callers
   must not run concurrently (the firmware picker and the app-SDK picker are
   mutually exclusive by construction). */
bool jpp_file_browser_run(
    const jpp_file_browser_io_t *io,
    const char *root_path,
    char *out_path,
    size_t out_len
);

#ifdef __cplusplus
}
#endif
