#include "../include/jpp_file_browser_core.h"
#include "../include/jpp_string_util.h"
#include "../include/jpp_ui_core.h"

#include <stdio.h>
#include <string.h>

#define FB_PAUSE_TICKS 3u   /* marquee hold at each end, in render ticks */

typedef struct {
    char     root[JPP_FB_PATH_MAX];
    char     dir[JPP_FB_PATH_MAX];
    char     names[JPP_FB_ENTRY_MAX][JPP_FB_NAME_MAX];
    bool     is_dir[JPP_FB_ENTRY_MAX];
    size_t   count;       /* entries in current dir (not counting "..") */
    bool     has_parent;  /* whether ".." row appears at logical index 0 */
    size_t   cursor;      /* logical cursor index */
    size_t   scroll;      /* logical index of first visible row */
    uint32_t marquee;     /* animation tick counter */
} fb_state_t;

static size_t fb_total(const fb_state_t *s)
{
    return s->count + (s->has_parent ? 1u : 0u);
}

static const char *fb_name(const fb_state_t *s, size_t i)
{
    if (s->has_parent) {
        if (i == 0u) { return ".."; }
        return s->names[i - 1u];
    }
    return s->names[i];
}

static bool fb_is_dir(const fb_state_t *s, size_t i)
{
    if (s->has_parent && i == 0u) { return true; }
    size_t ei = s->has_parent ? i - 1u : i;
    return s->is_dir[ei];
}

static void fb_load(fb_state_t *s, const jpp_file_browser_io_t *io)
{
    s->count = io->list_dir(io->ctx, s->dir, s->names, s->is_dir, JPP_FB_ENTRY_MAX);

    /* Bubble sort by display name — entry count is small, and qsort cannot
       carry the companion is_dir flags alongside the fixed-size name rows. */
    for (size_t i = 0u; i + 1u < s->count; i++) {
        for (size_t j = 0u; j + 1u < s->count - i; j++) {
            if (strcmp(s->names[j], s->names[j + 1u]) > 0) {
                char tmp[JPP_FB_NAME_MAX];
                memcpy(tmp, s->names[j], JPP_FB_NAME_MAX);
                memcpy(s->names[j], s->names[j + 1u], JPP_FB_NAME_MAX);
                memcpy(s->names[j + 1u], tmp, JPP_FB_NAME_MAX);
                bool bt            = s->is_dir[j];
                s->is_dir[j]       = s->is_dir[j + 1u];
                s->is_dir[j + 1u]  = bt;
            }
        }
    }

    s->has_parent = !jpp_str_eq(s->dir, s->root);
    s->cursor  = 0u;
    s->scroll  = 0u;
    s->marquee = 0u;
}

static void fb_go_up(fb_state_t *s, const jpp_file_browser_io_t *io)
{
    char *slash = strrchr(s->dir, '/');
    if (slash != NULL && slash != s->dir) {
        *slash = '\0';
    }
    fb_load(s, io);
}

bool jpp_file_browser_run(
    const jpp_file_browser_io_t *io,
    const char *root_path,
    char *out_path,
    size_t out_len)
{
    if (io == NULL || io->list_dir == NULL || io->render == NULL ||
        io->wait_key == NULL || root_path == NULL ||
        out_path == NULL || out_len == 0u) {
        return false;
    }

    static fb_state_t s;   /* static: avoid ~5 KB on the calling stack */
    memset(&s, 0, sizeof(s));
    jpp_str_copy(s.root, sizeof(s.root), root_path);
    jpp_str_copy(s.dir,  sizeof(s.dir),  root_path);
    fb_load(&s, io);

    for (;;) {
        size_t total = fb_total(&s);
        s.scroll = jpp_ui_scroll_clamp(s.cursor, total, JPP_FB_VISIBLE, s.scroll);

        char        rows[JPP_FB_VISIBLE][JPP_FB_ROW_MAX];
        const char *row_ptrs[JPP_FB_VISIBLE];
        size_t      n = 0u;
        for (size_t v = 0u; v < JPP_FB_VISIBLE; v++) {
            size_t idx = s.scroll + v;
            if (idx >= total) { break; }
            const char *name = fb_name(&s, idx);
            size_t off = jpp_ui_marquee_offset(s.marquee, strlen(name),
                                               JPP_FB_ITEM_CHARS, FB_PAUSE_TICKS);
            snprintf(rows[n], JPP_FB_ROW_MAX, "%c%.*s",
                     (idx == s.cursor) ? '>' : ' ',
                     (int)JPP_FB_ITEM_CHARS, name + off);
            row_ptrs[n] = rows[n];
            n++;
        }
        io->render(io->ctx, row_ptrs, n);

        jpp_fb_key_t key = io->wait_key(io->ctx, JPP_FB_MARQUEE_MS);
        if (key == JPP_FB_KEY_NONE) {
            s.marquee++;
            continue;
        }
        s.marquee = 0u;   /* reset animation on any key press */

        switch (key) {
        case JPP_FB_KEY_UP:
            if (s.cursor > 0u) { s.cursor--; }
            break;
        case JPP_FB_KEY_DOWN:
            if (s.cursor + 1u < total) { s.cursor++; }
            break;

        case JPP_FB_KEY_OK:
            if (total == 0u) { break; }
            if (fb_is_dir(&s, s.cursor)) {
                if (s.has_parent && s.cursor == 0u) {
                    fb_go_up(&s, io);
                } else {
                    /* Enter subdirectory — strip the trailing "/" from name. */
                    size_t ei = s.has_parent ? s.cursor - 1u : s.cursor;
                    char subname[JPP_FB_NAME_MAX];
                    jpp_str_copy(subname, sizeof(subname), s.names[ei]);
                    size_t slen = strlen(subname);
                    if (slen > 0u && subname[slen - 1u] == '/') {
                        subname[slen - 1u] = '\0';
                    }
                    char newdir[JPP_FB_PATH_MAX * 2u];
                    snprintf(newdir, sizeof(newdir), "%s/%s", s.dir, subname);
                    snprintf(s.dir, sizeof(s.dir), "%.*s",
                             (int)(sizeof(s.dir) - 1u), newdir);
                    fb_load(&s, io);
                }
            } else {
                /* File selected — build absolute path. */
                size_t ei = s.has_parent ? s.cursor - 1u : s.cursor;
                snprintf(out_path, out_len, "%s/%s", s.dir, s.names[ei]);
                return true;
            }
            break;

        case JPP_FB_KEY_BACK:
            if (s.has_parent) {
                fb_go_up(&s, io);
            } else {
                return false;   /* cancel */
            }
            break;

        default:
            break;
        }
    }
}
