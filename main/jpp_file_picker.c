#include "jpp_file_picker.h"
#include "ssd1306.h"
#include "jpp_app_dispatch.h"   /* for s_action_queue (extern QueueHandle_t) */

#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* ---- Limits ---------------------------------------------------------------- */

#define FP_ENTRY_MAX   32u   /* max entries per directory (excess silently dropped) */
#define FP_NAME_MAX    65u   /* max display-name length incl. "/" suffix and NUL    */
#define FP_PATH_MAX   256u   /* max absolute path length                            */
#define FP_VISIBLE      6u   /* list rows visible on screen (SSD1306 pages 2–7)    */
#define FP_ITEM_CHARS  20u   /* chars available for the name after the ">/" prefix  */

/* Marquee timing: 400 ms per tick, 3-tick pause at each end before scrolling. */
#define FP_MARQUEE_MS    400u
#define FP_MARQUEE_PAUSE   3u

/* ---- State ----------------------------------------------------------------- */

typedef struct {
    char   root[FP_PATH_MAX];
    char   dir[FP_PATH_MAX];
    char   names[FP_ENTRY_MAX][FP_NAME_MAX]; /* "name" or "name/" */
    bool   is_dir_flag[FP_ENTRY_MAX];
    size_t count;       /* entries in current dir (not counting "..") */
    bool   has_parent;  /* whether ".." row appears at logical index 0 */
    size_t cursor;      /* logical cursor index */
    size_t scroll;      /* logical index of first visible row */
    uint32_t marquee;   /* animation tick counter */
} fp_state_t;

/* ---- Helpers --------------------------------------------------------------- */

static size_t fp_total(const fp_state_t *s)
{
    return s->count + (s->has_parent ? 1u : 0u);
}

static const char *fp_name(const fp_state_t *s, size_t i)
{
    if (s->has_parent) {
        if (i == 0u) return "..";
        return s->names[i - 1u];
    }
    return s->names[i];
}

static bool fp_is_dir(const fp_state_t *s, size_t i)
{
    if (s->has_parent && i == 0u) return true;
    size_t ei = s->has_parent ? i - 1u : i;
    return s->is_dir_flag[ei];
}

/* Compare entry names for sorting (ascending). */
static int fp_name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void fp_load(fp_state_t *s)
{
    s->count = 0u;

    DIR *d = opendir(s->dir);
    if (d == NULL) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && s->count < FP_ENTRY_MAX) {
        if (ent->d_name[0] == '.') continue;  /* skip hidden / . / .. */

        bool is_dir_entry = (ent->d_type == DT_DIR);
        if (ent->d_type == DT_UNKNOWN) {
            char full[FP_PATH_MAX * 2];
            snprintf(full, sizeof(full), "%s/%s", s->dir, ent->d_name);
            struct stat st;
            if (stat(full, &st) == 0) {
                is_dir_entry = (bool)S_ISDIR(st.st_mode);
            }
        }

        size_t nlen = strlen(ent->d_name);
        /* Reserve space for "/" suffix and NUL. */
        if (nlen >= FP_NAME_MAX - 1u) { nlen = FP_NAME_MAX - 2u; }
        memcpy(s->names[s->count], ent->d_name, nlen);
        s->names[s->count][nlen] = '\0';
        if (is_dir_entry && nlen + 1u < FP_NAME_MAX) {
            s->names[s->count][nlen]     = '/';
            s->names[s->count][nlen + 1u] = '\0';
        }
        s->is_dir_flag[s->count] = is_dir_entry;
        s->count++;
    }
    closedir(d);

    /* Sort by display name (alphabetical; dirs have "/" so sort after letters). */
    if (s->count > 1u) {
        /* Bubble sort — entry count is small, qsort needs a flat array here. */
        for (size_t i = 0u; i < s->count - 1u; i++) {
            for (size_t j = 0u; j < s->count - 1u - i; j++) {
                if (fp_name_cmp(s->names[j], s->names[j + 1u]) > 0) {
                    char tmp[FP_NAME_MAX];
                    memcpy(tmp, s->names[j], FP_NAME_MAX);
                    memcpy(s->names[j], s->names[j + 1u], FP_NAME_MAX);
                    memcpy(s->names[j + 1u], tmp, FP_NAME_MAX);
                    bool bt = s->is_dir_flag[j];
                    s->is_dir_flag[j]       = s->is_dir_flag[j + 1u];
                    s->is_dir_flag[j + 1u]  = bt;
                }
            }
        }
    }

    s->has_parent = (strcmp(s->dir, s->root) != 0);
}

/* Navigate one level up from s->dir. */
static void fp_go_up(fp_state_t *s)
{
    char *slash = strrchr(s->dir, '/');
    if (slash != NULL && slash != s->dir) {
        *slash = '\0';
    }
    fp_load(s);
    s->cursor  = 0u;
    s->scroll  = 0u;
    s->marquee = 0u;
}

/* Compute the marquee display offset for a name of length namelen. */
static size_t fp_marquee_offset(uint32_t tick, size_t namelen)
{
    if (namelen <= FP_ITEM_CHARS) return 0u;
    size_t max_off = namelen - FP_ITEM_CHARS;
    size_t cycle   = 2u * FP_MARQUEE_PAUSE + max_off;
    size_t phase   = (size_t)(tick % (uint32_t)cycle);
    if (phase < FP_MARQUEE_PAUSE)                   return 0u;
    if (phase < FP_MARQUEE_PAUSE + max_off)         return phase - FP_MARQUEE_PAUSE;
    return max_off;
}

/* ---- Rendering ------------------------------------------------------------ */

static void fp_render(const fp_state_t *s)
{
    ssd1306_clear();
    ssd1306_draw_string(0u, 0u, "Select a file:", false);
    ssd1306_fill_page(1u, 0x08u);   /* divider */

    size_t total = fp_total(s);
    for (size_t v = 0u; v < FP_VISIBLE; v++) {
        size_t idx = s->scroll + v;
        if (idx >= total) break;

        bool        selected = (idx == s->cursor);
        const char *name     = fp_name(s, idx);
        size_t      namelen  = strlen(name);

        /* Compute the visible window into the name (marquee). */
        size_t off = fp_marquee_offset(s->marquee, namelen);

        char row[23];   /* ">" + 20 chars + NUL */
        snprintf(row, sizeof(row), "%c%.20s",
                 selected ? '>' : ' ',
                 name + off);
        ssd1306_draw_string((uint8_t)(2u + v), 0u, row, false);
    }

    ssd1306_flush();
}

/* ---- Public API ----------------------------------------------------------- */

bool jpp_file_picker(const char    *root_path,
                     jpp_ui_shell_t *shell,
                     char           *out_path,
                     size_t          out_len)
{
    if (root_path == NULL || out_path == NULL || out_len == 0u) return false;

    static fp_state_t s;   /* static: avoid ~5 KB on the calling stack */
    memset(&s, 0, sizeof(s));
    strncpy(s.root, root_path, FP_PATH_MAX - 1u);
    strncpy(s.dir,  root_path, FP_PATH_MAX - 1u);
    fp_load(&s);

    for (;;) {
        /* Clamp scroll so cursor is always visible. */
        size_t total = fp_total(&s);
        if (s.cursor < s.scroll) {
            s.scroll = s.cursor;
        } else if (total > FP_VISIBLE && s.cursor >= s.scroll + FP_VISIBLE) {
            s.scroll = s.cursor - FP_VISIBLE + 1u;
        }
        if (total >= FP_VISIBLE && s.scroll + FP_VISIBLE > total) {
            s.scroll = total - FP_VISIBLE;
        }

        fp_render(&s);

        /* Block for key or marquee tick. */
        jpp_ui_action_t action;
        bool got_key = (xQueueReceive(s_action_queue, &action,
                                       pdMS_TO_TICKS(FP_MARQUEE_MS)) == pdTRUE);

        if (shell != NULL) { jpp_ui_shell_note_activity(shell); }

        if (!got_key) {
            s.marquee++;
            continue;
        }

        s.marquee = 0u;   /* reset animation on any key press */

        switch (action) {
        case JPP_UI_ACTION_UP:
            if (s.cursor > 0u) { s.cursor--; }
            break;
        case JPP_UI_ACTION_DOWN:
            if (s.cursor + 1u < total) { s.cursor++; }
            break;

        case JPP_UI_ACTION_OK:
            if (total == 0u) break;
            if (fp_is_dir(&s, s.cursor)) {
                if (s.has_parent && s.cursor == 0u) {
                    fp_go_up(&s);
                } else {
                    /* Enter subdirectory — strip the trailing "/" from name. */
                    size_t ei = s.has_parent ? s.cursor - 1u : s.cursor;
                    char subname[FP_NAME_MAX];
                    strncpy(subname, s.names[ei], FP_NAME_MAX - 1u);
                    subname[FP_NAME_MAX - 1u] = '\0';
                    size_t slen = strlen(subname);
                    if (slen > 0u && subname[slen - 1u] == '/') {
                        subname[slen - 1u] = '\0';
                    }
                    char newdir[FP_PATH_MAX * 2];
                    snprintf(newdir, sizeof(newdir), "%s/%s", s.dir, subname);
                    strncpy(s.dir, newdir, FP_PATH_MAX - 1u);
                    s.dir[FP_PATH_MAX - 1u] = '\0';
                    fp_load(&s);
                    s.cursor  = 0u;
                    s.scroll  = 0u;
                    s.marquee = 0u;
                }
            } else {
                /* File selected — build absolute path. */
                size_t ei = s.has_parent ? s.cursor - 1u : s.cursor;
                snprintf(out_path, out_len, "%s/%s", s.dir, s.names[ei]);
                return true;
            }
            break;

        case JPP_UI_ACTION_BACK:
            if (s.has_parent) {
                fp_go_up(&s);
            } else {
                return false;   /* cancel */
            }
            break;

        default: break;
        }
    }
}
