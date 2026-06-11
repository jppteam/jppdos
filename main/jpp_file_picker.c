#include "jpp_file_picker.h"
#include "jpp_file_browser_core.h"
#include "jpp_draw_util.h"
#include "ssd1306.h"
#include "jpp_app_dispatch.h"   /* for s_action_queue (extern QueueHandle_t) */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Browser engine I/O: list directories via opendir/readdir, render to the
   SSD1306, and take keys from the firmware action queue. */

static size_t fp_list_dir(void *ctx, const char *abs_dir,
                          char names[][JPP_FB_NAME_MAX],
                          bool *is_dir, size_t max)
{
    (void)ctx;
    size_t count = 0u;

    DIR *d = opendir(abs_dir);
    if (d == NULL) { return 0u; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max) {
        if (ent->d_name[0] == '.') { continue; }  /* skip hidden / . / .. */

        bool is_dir_entry = (ent->d_type == DT_DIR);
        if (ent->d_type == DT_UNKNOWN) {
            char full[JPP_FB_PATH_MAX * 2u];
            snprintf(full, sizeof(full), "%s/%s", abs_dir, ent->d_name);
            struct stat st;
            if (stat(full, &st) == 0) {
                is_dir_entry = (bool)S_ISDIR(st.st_mode);
            }
        }

        size_t nlen = strlen(ent->d_name);
        /* Reserve space for "/" suffix and NUL. */
        if (nlen >= JPP_FB_NAME_MAX - 1u) { nlen = JPP_FB_NAME_MAX - 2u; }
        memcpy(names[count], ent->d_name, nlen);
        names[count][nlen] = '\0';
        if (is_dir_entry) {
            names[count][nlen]      = '/';
            names[count][nlen + 1u] = '\0';
        }
        is_dir[count] = is_dir_entry;
        count++;
    }
    closedir(d);
    return count;
}

static void fp_render(void *ctx, const char *const *rows, size_t row_count)
{
    (void)ctx;
    ssd1306_clear();
    jpp_draw_title("Select a file:");
    for (size_t v = 0u; v < row_count; v++) {
        ssd1306_draw_string((uint8_t)(2u + v), 0u, rows[v], false);
    }
    ssd1306_flush();
}

static jpp_fb_key_t fp_wait_key(void *ctx, uint32_t timeout_ms)
{
    jpp_ui_shell_t *shell = (jpp_ui_shell_t *)ctx;

    jpp_ui_action_t action;
    bool got_key = (xQueueReceive(s_action_queue, &action,
                                  pdMS_TO_TICKS(timeout_ms)) == pdTRUE);

    if (shell != NULL) { jpp_ui_shell_note_activity(shell); }

    if (!got_key) { return JPP_FB_KEY_NONE; }
    switch (action) {
    case JPP_UI_ACTION_UP:   return JPP_FB_KEY_UP;
    case JPP_UI_ACTION_DOWN: return JPP_FB_KEY_DOWN;
    case JPP_UI_ACTION_OK:   return JPP_FB_KEY_OK;
    case JPP_UI_ACTION_BACK: return JPP_FB_KEY_BACK;
    default:                 return JPP_FB_KEY_NONE;
    }
}

bool jpp_file_picker(const char     *root_path,
                     jpp_ui_shell_t *shell,
                     char           *out_path,
                     size_t          out_len)
{
    const jpp_file_browser_io_t io = {
        .list_dir = fp_list_dir,
        .render   = fp_render,
        .wait_key = fp_wait_key,
        .ctx      = shell,
    };
    return jpp_file_browser_run(&io, root_path, out_path, out_len);
}
