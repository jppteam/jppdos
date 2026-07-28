#include "meetapp.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpp_broker_core.h"
#include "jpp_crypto_core.h"
#include "sodium/randombytes.h"

#include "meetapp_identity.h"
#include "meetapp_ble.h"
#include "meetapp_proof.h"

static const char *TAG = "meetapp";

/* OLED text width in characters (status footer is one extra row beneath). */
#define MEETAPP_DISP_W 21u

/* ---- Small UI helpers ---------------------------------------------------- */

/* Copy `s` horizontally centred into a MEETAPP_DISP_W-wide field. dst must hold
   at least MEETAPP_DISP_W + 1 bytes. */
static void center_into(char *dst, const char *s)
{
    size_t len = strnlen(s, MEETAPP_DISP_W);
    size_t pad = (MEETAPP_DISP_W - len) / 2u;
    memset(dst, ' ', pad);
    memcpy(dst + pad, s, len);
    dst[pad + len] = '\0';
}

/* Set a text-only frame, clearing the pixel canvas first so leftovers from a
   previous canvas screen (e.g. the big peer count) don't bleed through. */
static void set_text_frame(jpp_sdk_context_t *ctx, const char *const *lines,
                           size_t n)
{
    jpp_sdk_canvas_clear(ctx);
    jpp_sdk_set_frame(ctx, lines, n);
}

/*
 * Clear only the top 40 canvas rows (content rows used by text and the big
 * peer-count number).  Rows 40-47 — the progress-bar strip — are left
 * untouched so bar_anim_task can own them without blinking.
 */
static void canvas_clear_top(jpp_sdk_context_t *ctx)
{
    static const uint8_t blank[JPP_SDK_CANVAS_ROW_BYTES]; /* BSS zero-init */
    for (uint8_t r = 0u; r < 40u; r++) {
        jpp_sdk_canvas_write(ctx, r, blank);
    }
}

/* 3x5 pixel font for digits 0-9; each row holds 3 bits, MSB = leftmost. */
static const uint8_t MEETAPP_DIGIT_FONT[10][5] = {
    { 0x7, 0x5, 0x5, 0x5, 0x7 }, /* 0 */
    { 0x2, 0x6, 0x2, 0x2, 0x7 }, /* 1 */
    { 0x7, 0x1, 0x7, 0x4, 0x7 }, /* 2 */
    { 0x7, 0x1, 0x7, 0x1, 0x7 }, /* 3 */
    { 0x5, 0x5, 0x7, 0x1, 0x1 }, /* 4 */
    { 0x7, 0x4, 0x7, 0x1, 0x7 }, /* 5 */
    { 0x7, 0x4, 0x7, 0x5, 0x7 }, /* 6 */
    { 0x7, 0x1, 0x2, 0x2, 0x2 }, /* 7 */
    { 0x7, 0x5, 0x7, 0x5, 0x7 }, /* 8 */
    { 0x7, 0x5, 0x7, 0x1, 0x7 }, /* 9 */
};

static void fill_block(jpp_sdk_context_t *ctx, int x, int y, int s)
{
    for (int dy = 0; dy < s; dy++) {
        for (int dx = 0; dx < s; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && px < 128 && py >= 0 && py < 48) {
                jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)px, (uint8_t)py, true);
            }
        }
    }
}

/* Draw `value` as large centred digits on the canvas, top edge at y_top. */
static void draw_big_number(jpp_sdk_context_t *ctx, size_t value, int y_top, int scale)
{
    char num[8];
    snprintf(num, sizeof(num), "%zu", value);
    size_t n = strlen(num);

    int glyph_w = 3 * scale;
    int gap     = scale;
    int total_w = (int)n * glyph_w + (int)(n - 1u) * gap;
    int x = (128 - total_w) / 2;

    for (size_t d = 0u; d < n; d++) {
        int digit = num[d] - '0';
        const uint8_t *g = MEETAPP_DIGIT_FONT[digit];
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 3; col++) {
                if (g[row] & (1u << (2 - col))) {
                    fill_block(ctx, x + col * scale, y_top + row * scale, scale);
                }
            }
        }
        x += glyph_w + gap;
    }
}

/*
 * Draw a real (pixel) progress bar into the canvas: an outlined track with a
 * filled block that ping-pongs left/right as `phase` advances. y_top is in
 * canvas coordinates (0 = first canvas row, i.e. screen page 2).
 */
static void draw_progress_bar(jpp_sdk_context_t *ctx, int y_top, unsigned phase)
{
    const int x0 = 4;
    const int x1 = 123;
    const int h  = 8;

    /* Outline. */
    for (int x = x0; x <= x1; x++) {
        jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)x, (uint8_t)y_top, true);
        jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)x, (uint8_t)(y_top + h - 1), true);
    }
    for (int y = y_top; y < y_top + h; y++) {
        jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)x0, (uint8_t)y, true);
        jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)x1, (uint8_t)y, true);
    }

    /* Sliding indicator block, inset by 2px from the outline. */
    int inner_lo = x0 + 2;
    int inner_hi = x1 - 2;
    int span     = inner_hi - inner_lo + 1;
    int chunk    = span / 3;
    int travel   = span - chunk;
    if (travel < 1) {
        travel = 1;
    }
    int p    = (int)(phase % (unsigned)(2 * travel));
    int left = inner_lo + ((p < travel) ? p : (2 * travel - p));
    for (int y = y_top + 2; y < y_top + h - 2; y++) {
        for (int x = left; x < left + chunk && x <= inner_hi; x++) {
            jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)x, (uint8_t)y, true);
        }
    }
}

/*
 * Draw a filled (determinate) progress bar: an outlined track filled from the
 * left in proportion to done/total.  Used by the stepper screen's bottom strip
 * so it shows overall protocol progress even when nothing is animating.
 */
static void draw_bar_determinate(jpp_sdk_context_t *ctx, int y_top,
                                 size_t done, size_t total)
{
    const int x0 = 4;
    const int x1 = 123;
    const int h  = 8;

    for (int x = x0; x <= x1; x++) {
        jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)x, (uint8_t)y_top, true);
        jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)x, (uint8_t)(y_top + h - 1), true);
    }
    for (int y = y_top; y < y_top + h; y++) {
        jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)x0, (uint8_t)y, true);
        jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)x1, (uint8_t)y, true);
    }

    int inner_lo = x0 + 2;
    int inner_hi = x1 - 2;
    int span     = inner_hi - inner_lo + 1;
    int fill     = (total == 0u) ? 0 : (int)((size_t)span * done / total);
    for (int y = y_top + 2; y < y_top + h - 2; y++) {
        for (int x = inner_lo; x < inner_lo + fill; x++) {
            jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)x, (uint8_t)y, true);
        }
    }
}

/*
 * Full-height "protocol progress" screen shared by every post-discovery signing
 * stage on both the leader and participant sides.  It fills all eight display
 * rows instead of leaving the bottom blank:
 *
 *   row 0   centred stage title            (e.g. "SIGNING")
 *   row 1   centred nickname subtitle
 *   row 2   canvas divider rule
 *   rows3-6 four-item checklist (done / active / pending)
 *   row 7   canvas progress bar (determinate here; an animated indeterminate
 *           bar from bar_anim_task overrides it during the long BLE waits)
 *
 * render_stepper is always called from the app task while no bar_anim_task is
 * running (each long wait stops its bar before the next stage renders), so it
 * fully clears the canvas; a bar task started afterwards then owns rows 40-47.
 */
#define MEETAPP_STEPPER_STEPS 4u

static void render_stepper(jpp_sdk_context_t *ctx, const char *title,
                           const char *nick,
                           const char *const *steps, size_t active)
{
    jpp_sdk_canvas_clear(ctx);   /* full clear — no bar task is running here */

    /* Divider rule across the middle of page 2, matching the bar margins. */
    for (int x = 4; x <= 123; x++) {
        jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)x, 4u, true);
    }

    /* Determinate fallback bar: half-fill the active step so a static stage
       still reads as "in progress" (an animated bar overrides it on waits). */
    draw_bar_determinate(ctx, 40, active * 2u + 1u, MEETAPP_STEPPER_STEPS * 2u);

    char t[MEETAPP_DISP_W + 1u];
    char nk[MEETAPP_DISP_W + 1u];
    center_into(t, title);
    center_into(nk, nick);

    char rows[MEETAPP_STEPPER_STEPS][MEETAPP_DISP_W + 8u];
    for (size_t i = 0u; i < MEETAPP_STEPPER_STEPS; i++) {
        const char *mark = (i < active) ? "[x]"
                         : (i == active) ? "[>]"
                                         : "[ ]";
        snprintf(rows[i], sizeof(rows[i]), " %s %s", mark, steps[i]);
    }

    const char *lp[7] = {
        t, nk, "", rows[0], rows[1], rows[2], rows[3],
    };
    jpp_sdk_set_frame(ctx, lp, 7u);
}

/* ---- Progress-bar animation task ---------------------------------------- */
/*
 * Runs at priority 2 (below the BLE app task at 4, above the display loop at
 * 1).  On single-core ESP32-C6 the BLE task always preempts this one, so
 * there is no concurrent canvas access and no mutex is required.
 *
 * The task owns canvas rows 40-47 (the 8-pixel bar strip).  It clears only
 * those rows and redraws the bar on every 100 ms tick, giving smooth 10 FPS
 * animation independently of however long the BLE operations take.
 */
typedef struct {
    jpp_sdk_context_t *ctx;
    volatile uint32_t  phase;
    volatile bool      stop;
} bar_anim_t;

static void bar_anim_task(void *arg)
{
    static const uint8_t blank[JPP_SDK_CANVAS_ROW_BYTES]; /* BSS zero-init */
    bar_anim_t *a = (bar_anim_t *)arg;
    while (!a->stop) {
        for (uint8_t r = 40u; r < 48u; r++) {
            jpp_sdk_canvas_write(a->ctx, r, blank);
        }
        draw_progress_bar(a->ctx, 40u, (unsigned)a->phase);
        a->phase += 7u;            /* 70 px/s at 100 ms ticks ≈ 2.2 s/cycle */
        vTaskDelay(pdMS_TO_TICKS(100u));
    }
    vTaskDelete(NULL);
}

static TaskHandle_t bar_anim_start(bar_anim_t *a, jpp_sdk_context_t *ctx)
{
    a->ctx   = ctx;
    a->phase = 0u;
    a->stop  = false;
    TaskHandle_t h = NULL;
    xTaskCreate(bar_anim_task, "bar_anim", 2048u, a, tskIDLE_PRIORITY + 2u, &h);
    return h;
}

static void anim_stop_wait(volatile bool *stop_flag, TaskHandle_t h)
{
    *stop_flag = true;
    while (eTaskGetState(h) != eDeleted) {
        vTaskDelay(pdMS_TO_TICKS(10u));
    }
}

static void bar_anim_stop(bar_anim_t *a, TaskHandle_t h)
{
    anim_stop_wait(&a->stop, h);
}

static uint32_t now_ms(void);
static void render_initiate(jpp_sdk_context_t *, const char *, size_t, unsigned);

/*
 * Extended animation task for the "MEETUP INITIATED" screen.  Handles the
 * full render (big peer count, countdown text, progress bar) at 10 FPS so the
 * countdown advances at consistent one-second intervals regardless of the BLE
 * loop cadence.
 */
typedef struct {
    jpp_sdk_context_t *ctx;
    const char        *nick;
    uint32_t           start_ms;
    uint32_t           window_ms;
    volatile size_t    peer_count;
    volatile uint32_t  phase;
    volatile bool      stop;
} initiate_anim_t;

static void initiate_anim_task(void *arg)
{
    static const uint8_t blank[JPP_SDK_CANVAS_ROW_BYTES];
    initiate_anim_t *a = (initiate_anim_t *)arg;
    while (!a->stop) {
        uint32_t elapsed  = now_ms() - a->start_ms;
        unsigned remain_s = (elapsed >= a->window_ms) ? 0u
                          : (unsigned)((a->window_ms - elapsed + 999u) / 1000u);
        render_initiate(a->ctx, a->nick, a->peer_count, remain_s);
        for (uint8_t r = 40u; r < 48u; r++) {
            jpp_sdk_canvas_write(a->ctx, r, blank);
        }
        draw_progress_bar(a->ctx, 40u, (unsigned)a->phase);
        a->phase += 7u;
        vTaskDelay(pdMS_TO_TICKS(100u));
    }
    vTaskDelete(NULL);
}

static uint32_t now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* ---- Full-height result screen (replaces the plain modal dialog) --------- */

static int iabs_(int v) { return v < 0 ? -v : v; }

/* Bounds-checked canvas pixel (windowed 0-47). */
static void canvas_px(jpp_sdk_context_t *ctx, int x, int y, bool on)
{
    if (x >= 0 && x < 128 && y >= 0 && y < (int)JPP_SDK_CANVAS_ROWS) {
        jpp_sdk_canvas_draw_pixel(ctx, (uint8_t)x, (uint8_t)y, on);
    }
}

/* Word-wrap `text` into up to max_lines rows of MEETAPP_DISP_W columns. */
static size_t wrap_msg(const char *text, char out[][MEETAPP_DISP_W + 1u],
                       size_t max_lines)
{
    size_t line = 0u, col = 0u;
    if (max_lines == 0u) {
        return 0u;
    }
    out[0][0] = '\0';
    for (size_t i = 0u; text[i] != '\0' && line < max_lines; ) {
        if (text[i] == ' ') { i++; continue; }
        size_t wl = 0u;
        while (text[i + wl] != '\0' && text[i + wl] != ' ') { wl++; }
        size_t need = (col > 0u ? 1u : 0u) + wl;
        if (col > 0u && col + need > MEETAPP_DISP_W) {
            if (++line >= max_lines) { break; }
            out[line][0] = '\0'; col = 0u;
        }
        if (col > 0u) { out[line][col++] = ' '; }
        for (size_t k = 0u; k < wl && col < MEETAPP_DISP_W; k++) {
            out[line][col++] = text[i + k];
        }
        out[line][col] = '\0';
        i += wl;
    }
    return (col > 0u || line > 0u) ? line + 1u : 0u;
}

static void draw_ring(jpp_sdk_context_t *ctx, int cx, int cy, int r)
{
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            int d = x * x + y * y;
            if (d <= r * r && d >= (r - 2) * (r - 2)) {
                canvas_px(ctx, cx + x, cy + y, true);
            }
        }
    }
}

/* A 2px-thick dot, top-left anchored. */
static void plot_thick(jpp_sdk_context_t *ctx, int x, int y)
{
    canvas_px(ctx, x, y, true);
    canvas_px(ctx, x + 1, y, true);
    canvas_px(ctx, x, y + 1, true);
}

static void draw_seg(jpp_sdk_context_t *ctx, int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int n = (iabs_(dx) > iabs_(dy)) ? iabs_(dx) : iabs_(dy);
    if (n == 0) { n = 1; }
    for (int i = 0; i <= n; i++) {
        plot_thick(ctx, x0 + dx * i / n, y0 + dy * i / n);
    }
}

static void draw_check(jpp_sdk_context_t *ctx, int cx, int cy)
{
    draw_seg(ctx, cx - 6, cy, cx - 2, cy + 5);
    draw_seg(ctx, cx - 2, cy + 5, cx + 7, cy - 6);
}

static void draw_bang(jpp_sdk_context_t *ctx, int cx, int cy)
{
    for (int y = cy - 7; y <= cy + 2; y++) {
        canvas_px(ctx, cx, y, true);
        canvas_px(ctx, cx + 1, y, true);
    }
    for (int y = cy + 5; y <= cy + 6; y++) {
        canvas_px(ctx, cx, y, true);
        canvas_px(ctx, cx + 1, y, true);
    }
}

/*
 * Full-height outcome screen: a centred headline, a check/warning badge on the
 * canvas, the wrapped detail message, and a bottom rule — the terminal-state
 * counterpart to render_stepper, filling all eight rows instead of the plain
 * top-aligned dialog.
 */
static void render_result(jpp_sdk_context_t *ctx, bool ok,
                          const char *headline, const char *msg)
{
    jpp_sdk_canvas_clear(ctx);
    for (int x = 4; x <= 123; x++) {
        canvas_px(ctx, x, 43, true);        /* bottom rule (page 7) */
    }
    const int cx = 64, cy = 12, r = 10;      /* badge in the upper-middle */
    draw_ring(ctx, cx, cy, r);
    if (ok) { draw_check(ctx, cx, cy); } else { draw_bang(ctx, cx, cy); }

    char hl[MEETAPP_DISP_W + 1u];
    center_into(hl, headline);
    char body[2][MEETAPP_DISP_W + 1u];
    size_t nb = wrap_msg(msg, body, 2u);
    char m0[MEETAPP_DISP_W + 1u];
    char m1[MEETAPP_DISP_W + 1u];
    center_into(m0, nb > 0u ? body[0] : "");
    center_into(m1, nb > 1u ? body[1] : "Press OK");

    /* headline on row 0; badge on canvas rows for pages 2-4; message on rows
       5-6 (page 5-6); rows 1-4 stay empty so the badge shows through. */
    const char *lp[7] = { hl, "", "", "", "", m0, m1 };
    jpp_sdk_set_frame(ctx, lp, 7u);
}

/* Show a full-height outcome screen the user acknowledges with OK. */
static void show_result(jpp_sdk_context_t *ctx, bool ok,
                        const char *headline, const char *msg)
{
    render_result(ctx, ok, headline, msg);
    while (true) {
        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        jpp_sdk_wait_key(ctx, 0u, &key);
        if (key == JPP_SDK_KEY_CENTER || key == JPP_SDK_KEY_CENTER_LONG) {
            break;
        }
    }
}

/* Error/attention outcome (warning badge). Keeps the old notify() call sites. */
static void notify(jpp_sdk_context_t *ctx, const char *title, const char *text)
{
    show_result(ctx, false, title, text);
}

/* Success outcome (check badge). */
static void notify_ok(jpp_sdk_context_t *ctx, const char *title, const char *text)
{
    show_result(ctx, true, title, text);
}

/* ---- Main menu ----------------------------------------------------------- */

/*
 * Render a simple selectable list with a ">"/blank cursor (matching the rest of
 * the OS) and a footer line. Returns the chosen index, or -1 if the user backed
 * out (long-press CENTER).
 */
static int menu_select(jpp_sdk_context_t *ctx, const char *title,
                       const char *const *items, size_t n)
{
    size_t sel = 0u;
    while (true) {
        char rows[8][MEETAPP_DISP_W + 4u];
        const char *lp[8];
        size_t r = 2u;

        snprintf(rows[0], sizeof(rows[r]), "%s", title);
        lp[0] = rows[0];
        lp[1] = "";

        for (size_t i = 0u; i < n && r < 7u; i++) {
            snprintf(rows[r], sizeof(rows[r]), "%c %s",
                     (i == sel) ? '>' : ' ', items[i]);
            lp[r] = rows[r];
            r++;
        }
        set_text_frame(ctx, lp, r);

        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        jpp_sdk_wait_key(ctx, 0u, &key);
        switch (key) {
        case JPP_SDK_KEY_UP:
            sel = (sel == 0u) ? (n - 1u) : (sel - 1u);
            break;
        case JPP_SDK_KEY_DOWN:
            sel = (sel + 1u) % n;
            break;
        case JPP_SDK_KEY_CENTER:
            return (int)sel;
        case JPP_SDK_KEY_CENTER_LONG:
            return -1;
        default:
            break;
        }
    }
}

/* ---- Reset-identity confirmation ----------------------------------------- */

/*
 * Confirmation screen for "Reset identity". The "Yes" option carries a live 5s
 * countdown and cannot be chosen until it elapses. Returns true if the user
 * confirmed, false if they cancelled or backed out.
 */
static bool confirm_reset(jpp_sdk_context_t *ctx)
{
    const uint32_t hold_ms = 5000u;
    uint32_t start = now_ms();
    size_t sel = 0u;  /* 0 = Yes, 1 = No */

    while (true) {
        uint32_t elapsed = now_ms() - start;
        bool ready = (elapsed >= hold_ms);
        unsigned remain_s = ready ? 0u : (unsigned)((hold_ms - elapsed + 999u) / 1000u);

        char yes[MEETAPP_DISP_W + 8u];
        char no[MEETAPP_DISP_W + 4u];
        if (ready) {
            snprintf(yes, sizeof(yes), "%c Yes, I'm sure", sel == 0u ? '>' : ' ');
        } else {
            snprintf(yes, sizeof(yes), "%c Yes, I'm sure (%us)",
                     sel == 0u ? '>' : ' ', remain_s);
        }
        snprintf(no, sizeof(no), "%c No, cancel", sel == 1u ? '>' : ' ');

        const char *lp[7] = {
            "MeetApp",
            "Reset your identity?",
            "You'll be able to set",
            "a new nickname after.",
            "",
            yes,
            no,
        };
        set_text_frame(ctx, lp, 7u);

        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        jpp_sdk_wait_key(ctx, 200u, &key);   /* 200ms so the countdown ticks */
        switch (key) {
        case JPP_SDK_KEY_UP:
        case JPP_SDK_KEY_DOWN:
            sel ^= 1u;
            break;
        case JPP_SDK_KEY_CENTER:
            if (sel == 1u) {
                return false;             /* No, cancel */
            }
            if (sel == 0u && ready) {
                return true;              /* Yes — only after the countdown */
            }
            break;                        /* Yes pressed too early: ignore */
        case JPP_SDK_KEY_CENTER_LONG:
            return false;
        default:
            break;                        /* timeout → re-render countdown */
        }
    }
}

/* ---- Timestamp from RTC -------------------------------------------------- */

static void get_timestamp(jpp_sdk_context_t *ctx, char *buf, size_t size)
{
    jpp_broker_result_t result;
    if (jpp_sdk_get_time(ctx, &result) == JPP_SDK_STATUS_OK && result.ok) {
        const char *ts = jpp_broker_result_get(&result, "text");
        if (ts != NULL) {
            strncpy(buf, ts, size - 1u);
            buf[size - 1u] = '\0';
            return;
        }
    }
    strncpy(buf, "0000-00-00 00:00", size - 1u);
    buf[size - 1u] = '\0';
}

/* ---- Build proof participant list in all_pubkeys order ------------------- */

static void build_participant_list(
    const meetapp_identity_t  *identity,
    const uint8_t             *all_pubkeys,
    size_t                     n_total,
    const meetapp_session_t   *session,    /* NULL for participant path */
    meetapp_proof_participant_t *out,
    size_t                    *out_count)
{
    *out_count = 0u;
    for (size_t i = 0u; i < n_total && i < MEETAPP_MAX_TOTAL; i++) {
        const uint8_t *pk = &all_pubkeys[i * 32u];
        memcpy(out[i].pubkey, pk, 32u);

        if (memcmp(pk, identity->pubkey, 32u) == 0) {
            /* This is our own entry. */
            strncpy(out[i].nickname, identity->nickname, MEETAPP_NICKNAME_MAX);
            out[i].nickname[MEETAPP_NICKNAME_MAX] = '\0';
        } else if (session != NULL) {
            /* Leader path: look up nickname from session->peers. */
            bool found = false;
            for (size_t j = 0u; j < session->peer_count; j++) {
                if (memcmp(session->peers[j].pubkey, pk, 32u) == 0) {
                    strncpy(out[i].nickname, session->peers[j].nickname,
                            MEETAPP_NICKNAME_MAX);
                    out[i].nickname[MEETAPP_NICKNAME_MAX] = '\0';
                    found = true;
                    break;
                }
            }
            if (!found) {
                snprintf(out[i].nickname, MEETAPP_NICKNAME_MAX + 1u,
                         "Peer%zu", i + 1u);
            }
        } else {
            /* Participant path: no nicknames for non-self entries. */
            snprintf(out[i].nickname, MEETAPP_NICKNAME_MAX + 1u,
                     "Peer%zu", i + 1u);
        }
        (*out_count)++;
    }
}

/* ---- Initiate (leader) flow ---------------------------------------------- */

/* Render the live "MEETUP INITIATED" discovery screen. */
static void render_initiate(jpp_sdk_context_t *ctx, const char *nick,
                            size_t peers, unsigned remain_s)
{
    canvas_clear_top(ctx);               /* leaves bar rows 40-47 untouched */
    draw_big_number(ctx, peers, 2, 4);   /* large centred peer count */
    /* progress bar is owned by bar_anim_task */

    char title[MEETAPP_DISP_W + 1u];
    char wait[MEETAPP_DISP_W + 1u];
    char nk[MEETAPP_DISP_W + 1u];
    char ok[MEETAPP_DISP_W + 1u];
    char wbuf[MEETAPP_DISP_W + 8u];

    center_into(title, "MEETUP INITIATED");
    snprintf(wbuf, sizeof(wbuf), "Waiting peers (%us)", remain_s);
    center_into(wait, wbuf);
    center_into(nk, nick);
    center_into(ok, "Press OK to finish");

    /* Rows 2-4 (the canvas big-number band) and row 7 (the canvas bar) are left
       to the pixel canvas; footer is empty so it doesn't paint over the bar. */
    const char *lp[7] = { title, wait, "", "", "", nk, ok };
    jpp_sdk_set_frame(ctx, lp, 7u);
}

static void run_leader(jpp_sdk_context_t *ctx,
                       const meetapp_identity_t *identity)
{
    static meetapp_session_t session;
    memset(&session, 0, sizeof(session));

    randombytes_buf(session.session_nonce, 8u);

    jpp_broker_result_t br;

    uint8_t ping_ad[31];
    size_t  ping_ad_len;
    meetapp_ble_build_ad(MEETAPP_AD_PING, session.session_nonce,
                         identity->nickname, ping_ad, &ping_ad_len);

    /* ble.advertise is front-loaded at startup, so this normally passes without a
       prompt; it stays as a denial guard, catching a declined grant before any
       background work is spawned. */
    if (jpp_sdk_ble_advertise_start(ctx, ping_ad, ping_ad_len, &br) ==
            JPP_SDK_STATUS_ACCESS_DENIED) {
        notify(ctx, "Permission needed", "Bluetooth access was denied.");
        return;
    }
    jpp_sdk_ble_advertise_stop(ctx, &br);

    /* Discovery: alternate short PING adverts and scans so the screen can show a
       live peer count, countdown and animation, and respond to OK / long-press. */
    const uint32_t window_ms = 60000u;
    uint32_t start = now_ms();
    bool proceed   = false;
    bool cancelled = false;
    initiate_anim_t anim = {
        .ctx        = ctx,
        .nick       = identity->nickname,
        .start_ms   = start,
        .window_ms  = window_ms,
        .peer_count = 0u,
        .phase      = 0u,
        .stop       = false,
    };
    TaskHandle_t anim_h;
    xTaskCreate(initiate_anim_task, "bar_anim", 2048u, &anim,
                tskIDLE_PRIORITY + 2u, &anim_h);

    while (true) {
        if (now_ms() - start >= window_ms) {
            break;
        }

        jpp_sdk_ble_advertise_start(ctx, ping_ad, ping_ad_len, &br);
        vTaskDelay(pdMS_TO_TICKS(350u));
        jpp_sdk_ble_advertise_stop(ctx, &br);

        size_t old_peer_count = session.peer_count;
        meetapp_ble_scan_pongs(ctx, &session, 400u);
        anim.peer_count = session.peer_count;

        if (old_peer_count != session.peer_count) {
            jpp_sdk_buzzer_play(ctx, JPP_BUZZER_SOUND_NOTIFY);
        }

        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        jpp_sdk_poll_key(ctx, &key);
        if (key == JPP_SDK_KEY_CENTER && session.peer_count > 0u) {
            proceed = true;
            break;
        }
        if (key == JPP_SDK_KEY_CENTER_LONG) {
            cancelled = true;
            break;
        }
    }

    anim_stop_wait(&anim.stop, anim_h);
    if (cancelled) { return; }

    if (!proceed && session.peer_count == 0u) {
        notify(ctx, "No peers found",
               "No nearby devices joined the meetup.");
        return;
    }

    /* Confirm the collected peers before signing; BACK aborts. */
    char confirm_text[64];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(confirm_text, sizeof(confirm_text),
             "%zu peer(s) in session. Start signing?", session.peer_count);
#pragma GCC diagnostic pop
    {
        jpp_sdk_ui_result_t res = JPP_SDK_UI_BACK;
        jpp_sdk_dialog(ctx, "Collect signatures", confirm_text, &res);
        if (res != JPP_SDK_UI_OK) {
            return;
        }
    }

    /* Optional leader comment shown at the top of the proof. It is part of the
       signed message and forwarded to every peer in round-1.5, so all rebuilt
       proofs hash to what was signed. BACK or an empty entry means "no comment". */
    char comment[MEETAPP_COMMENT_MAX + 1u];
    comment[0] = '\0';
    {
        jpp_sdk_ui_result_t res = JPP_SDK_UI_BACK;
        jpp_sdk_input(ctx, "Add comment?", NULL, JPP_SDK_INPUT_TEXT,
                      comment, sizeof(comment), &res);
        if (res == JPP_SDK_UI_BACK) {
            comment[0] = '\0';
        }
    }

    /* Generate own MuSig2 nonce. */
    uint8_t my_secnonce[JPP_CRYPTO_MUSIG2_SECNONCE_BYTES];
    uint8_t my_pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES];
    if (jpp_crypto_musig2_nonce_gen(my_secnonce, my_pubnonce) != JPP_CRYPTO_OK) {
        notify(ctx, "Error", "Could not generate signing nonce.");
        return;
    }

    static const char *const L_STEPS[MEETAPP_STEPPER_STEPS] = {
        "Gather nonces", "Sign proof", "Deliver proof", "Save proof",
    };
    bar_anim_t   sanim;
    TaskHandle_t sanim_h;

    /* Round A: collect each peer's pubkey + pubnonce via GATT. */
    render_stepper(ctx, "SIGNING", identity->nickname, L_STEPS, 0u);
    sanim_h = bar_anim_start(&sanim, ctx);
    meetapp_ble_collect_nonces(ctx, identity, &session);
    bar_anim_stop(&sanim, sanim_h);

    /* Build linearised all_pubkeys: leader first, then peers in order. */
    size_t n_total = 1u + session.peer_count;
    static uint8_t all_pubkeys[MEETAPP_MAX_TOTAL * JPP_CRYPTO_PUBKEY_BYTES];
    memcpy(all_pubkeys, identity->pubkey, 32u);
    for (size_t i = 0u; i < session.peer_count; i++) {
        memcpy(&all_pubkeys[(1u + i) * 32u], session.peers[i].pubkey, 32u);
    }

    /* Build linearised all_pubnonces: leader first, then peers. */
    static uint8_t all_pubnonces[MEETAPP_MAX_TOTAL * JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES];
    memcpy(all_pubnonces, my_pubnonce, 64u);
    for (size_t i = 0u; i < session.peer_count; i++) {
        memcpy(&all_pubnonces[(1u + i) * 64u], session.peers[i].pubnonce, 64u);
    }

    /* Aggregate nonces. */
    uint8_t agg_pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES];
    if (jpp_crypto_musig2_aggregate_nonces(all_pubnonces, n_total,
                                           agg_pubnonce) != JPP_CRYPTO_OK) {
        notify(ctx, "Error", "Could not aggregate nonces.");
        return;
    }

    /* Get timestamp. */
    char timestamp[20];
    get_timestamp(ctx, timestamp, sizeof(timestamp));

    /* Build proof message and compute hash. */
    static meetapp_proof_participant_t participants[MEETAPP_MAX_TOTAL];
    size_t n_parts = 0u;
    build_participant_list(identity, all_pubkeys, n_total, &session,
                           participants, &n_parts);

    static char msg_text[MEETAPP_PROOF_MSG_MAX];
    uint8_t msg_hash[64];
    meetapp_proof_build_message(n_parts, participants, session.session_nonce,
                                timestamp, comment, msg_text, sizeof(msg_text),
                                msg_hash);

    /* Leader partial sig. */
    uint8_t my_partial_sig[JPP_CRYPTO_MUSIG2_PARTIAL_SIG_BYTES];
    if (jpp_crypto_musig2_partial_sign(my_secnonce, identity->seckey,
                                       all_pubkeys, n_total,
                                       agg_pubnonce,
                                       msg_hash, 64u,
                                       my_partial_sig) != JPP_CRYPTO_OK) {
        notify(ctx, "Error", "Could not create partial signature.");
        return;
    }

    /* Round B: send round-1.5 to each peer and collect their partial sigs. */
    render_stepper(ctx, "SIGNING", identity->nickname, L_STEPS, 1u);
    sanim_h = bar_anim_start(&sanim, ctx);
    meetapp_ble_exchange_sigs(ctx, &session, msg_hash, agg_pubnonce,
                               all_pubkeys, n_total,
                               timestamp, comment, participants);
    bar_anim_stop(&sanim, sanim_h);

    /* Aggregate all partial sigs: [leader, peer0, peer1, ...] */
    static uint8_t all_partial[MEETAPP_MAX_TOTAL * JPP_CRYPTO_MUSIG2_PARTIAL_SIG_BYTES];
    memcpy(all_partial, my_partial_sig, 32u);
    for (size_t i = 0u; i < session.peer_count; i++) {
        memcpy(&all_partial[(1u + i) * 32u], session.peers[i].partial_sig, 32u);
    }

    uint8_t final_sig[JPP_CRYPTO_SIG_BYTES];
    memcpy(final_sig, agg_pubnonce, 32u); /* R_agg in first 32 bytes */
    if (jpp_crypto_musig2_aggregate_sigs(all_partial, n_total,
                                          final_sig) != JPP_CRYPTO_OK) {
        notify(ctx, "Error", "Could not aggregate signatures.");
        return;
    }

    /* Round C: distribute final sig to each peer. */
    render_stepper(ctx, "SIGNING", identity->nickname, L_STEPS, 2u);
    sanim_h = bar_anim_start(&sanim, ctx);
    meetapp_ble_distribute_sig(ctx, &session, final_sig);
    bar_anim_stop(&sanim, sanim_h);

    /* Save proof locally. */
    render_stepper(ctx, "SIGNING", identity->nickname, L_STEPS, 3u);
    bool saved = meetapp_proof_save(ctx, n_parts, participants,
                                     session.session_nonce, timestamp, comment,
                                     final_sig);

    char ts_short[12];
    strncpy(ts_short, timestamp, 11u);
    ts_short[11] = '\0';

    if (saved) {
        jpp_sdk_buzzer_play(ctx, JPP_BUZZER_SOUND_SUCCESS);
        char done_msg[64];
        snprintf(done_msg, sizeof(done_msg), "Signed %s", ts_short);
        notify_ok(ctx, "Proof complete", done_msg);
    } else {
        notify(ctx, "Save failed", "Proof signed but could not be saved.");
    }
}

/* ---- Join (participant) flow --------------------------------------------- */

/* Render the live "SCANNING FOR PING" screen. */
static void render_join(jpp_sdk_context_t *ctx, const char *nick)
{
    canvas_clear_top(ctx);               /* leaves bar rows 40-47 untouched */
    /* progress bar is owned by bar_anim_task */

    char title[MEETAPP_DISP_W + 1u];
    char nk[MEETAPP_DISP_W + 1u];
    char cancel[MEETAPP_DISP_W + 1u];

    center_into(title, "SCANNING FOR PING");
    center_into(nk, nick);
    center_into(cancel, "Hold OK to cancel");

    const char *lp[7] = { "", title, "", nk, cancel, "" };
    jpp_sdk_set_frame(ctx, lp, 7u);
}

static void run_participant(jpp_sdk_context_t *ctx,
                             const meetapp_identity_t *identity)
{
    uint8_t session_nonce[8];
    char    leader_addr[JPP_SDK_BLE_ADDR_LEN];

    /* ble.scan is front-loaded at startup; this stays as a denial guard so a
       declined grant is caught before the animation task starts. */
    {
        size_t dummy = 0u;
        jpp_broker_result_t probe;
        if (jpp_sdk_ble_scan(ctx, 0u, NULL, 0u, &dummy, &probe) ==
                JPP_SDK_STATUS_ACCESS_DENIED) {
            notify(ctx, "Permission needed", "Bluetooth access was denied.");
            return;
        }
    }

    /* Scan in slices so the screen can animate and a long-press can cancel. */
    const uint32_t window_ms = 120000u;
    uint32_t start = now_ms();
    bool found     = false;
    bool cancelled = false;
    bar_anim_t anim;
    TaskHandle_t anim_h = bar_anim_start(&anim, ctx);

    while (now_ms() - start < window_ms) {
        render_join(ctx, identity->nickname);
        if (meetapp_ble_scan_ping_once(ctx, session_nonce, leader_addr, 600u)) {
            found = true;
            break;
        }
        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        jpp_sdk_poll_key(ctx, &key);
        if (key == JPP_SDK_KEY_CENTER_LONG) {
            cancelled = true;
            break;
        }
    }

    bar_anim_stop(&anim, anim_h);
    if (cancelled) { return; }

    if (!found) {
        notify(ctx, "No leader found",
               "No meetup leader was detected nearby.");
        return;
    }

    jpp_broker_result_t br;

    static const char *const P_STEPS[MEETAPP_STEPPER_STEPS] = {
        "Link to leader", "Exchange keys", "Sign proof", "Get final proof",
    };
    bar_anim_t   sanim;
    TaskHandle_t sanim_h;

    render_stepper(ctx, "SIGNING", identity->nickname, P_STEPS, 0u);

    /* Claim the GATT host role so the leader can connect and exchange data. */
    {
        jpp_sdk_status_t reg = jpp_sdk_ble_service_register(
            ctx, JPP_SDK_BLE_HOST_SVC_UUID, &br);
        if (reg == JPP_SDK_STATUS_ACCESS_DENIED) {
            notify(ctx, "Permission needed", "Bluetooth host access was denied.");
            return;
        }
        if (reg != JPP_SDK_STATUS_OK) {
            notify(ctx, "Error", "Could not start the BLE host service.");
            return;
        }
    }

    /* Generate MuSig2 nonce. */
    uint8_t my_secnonce[JPP_CRYPTO_MUSIG2_SECNONCE_BYTES];
    uint8_t my_pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES];
    jpp_crypto_musig2_nonce_gen(my_secnonce, my_pubnonce);

    /* Publish TX value: [MEETAPP_TX_NONCE][pubkey[32]][pubnonce[64]]. */
    {
        uint8_t tx[97];
        tx[0] = MEETAPP_TX_NONCE;
        memcpy(&tx[1],  identity->pubkey, 32u);
        memcpy(&tx[33], my_pubnonce,       64u);
        jpp_sdk_ble_host_set_value(ctx, tx, sizeof(tx), &br);
    }
    jpp_sdk_ble_host_clear(ctx, &br);

    /* Start connectable PONG advertisement. */
    jpp_sdk_ble_set_connectable(ctx, true, &br);
    uint8_t pong_ad[31];
    size_t  pong_ad_len;
    meetapp_ble_build_ad(MEETAPP_AD_PONG, session_nonce,
                         identity->nickname, pong_ad, &pong_ad_len);
    jpp_sdk_ble_advertise_start(ctx, pong_ad, pong_ad_len, &br);

    /* Wait for round-1.5 (leader connects and writes the RX characteristic).
       This can block for a while, so animate the bottom bar over the stepper. */
    render_stepper(ctx, "SIGNING", identity->nickname, P_STEPS, 1u);
    static uint8_t all_pubkeys[MEETAPP_MAX_TOTAL * JPP_CRYPTO_PUBKEY_BYTES];
    uint8_t msg_hash[64];
    uint8_t agg_pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES];
    size_t  n_total = 0u;
    char        leader_timestamp[MEETAPP_TS_FIELD_LEN];
    static char peer_nicknames[MEETAPP_MAX_TOTAL][MEETAPP_NICK_FIELD_LEN];
    char        leader_comment[MEETAPP_COMMENT_FIELD_LEN];

    sanim_h = bar_anim_start(&sanim, ctx);
    bool got_round15 = meetapp_ble_wait_round15(ctx, msg_hash, agg_pubnonce,
                                                all_pubkeys, &n_total,
                                                leader_timestamp, peer_nicknames,
                                                leader_comment, 120000u);
    bar_anim_stop(&sanim, sanim_h);
    if (!got_round15) {
        jpp_sdk_ble_advertise_stop(ctx, &br);
        jpp_sdk_ble_service_unregister(ctx, &br);
        notify(ctx, "Timed out", "No signing request from the leader.");
        return;
    }

    /* Compute partial signature (brief). */
    render_stepper(ctx, "SIGNING", identity->nickname, P_STEPS, 2u);
    uint8_t my_partial_sig[JPP_CRYPTO_MUSIG2_PARTIAL_SIG_BYTES];
    if (jpp_crypto_musig2_partial_sign(my_secnonce, identity->seckey,
                                       all_pubkeys, n_total,
                                       agg_pubnonce,
                                       msg_hash, 64u,
                                       my_partial_sig) != JPP_CRYPTO_OK) {
        jpp_sdk_ble_advertise_stop(ctx, &br);
        jpp_sdk_ble_service_unregister(ctx, &br);
        notify(ctx, "Error", "Signing failed.");
        return;
    }

    /* Update TX value with the partial sig. */
    {
        uint8_t tx[33];
        tx[0] = MEETAPP_TX_PARTSIG;
        memcpy(&tx[1], my_partial_sig, 32u);
        jpp_sdk_ble_host_set_value(ctx, tx, sizeof(tx), &br);
    }

    /* Wait for the final signature (long): animate the bottom bar again. */
    render_stepper(ctx, "SIGNING", identity->nickname, P_STEPS, 3u);
    uint8_t final_sig[JPP_CRYPTO_SIG_BYTES];
    sanim_h = bar_anim_start(&sanim, ctx);
    bool got_final = meetapp_ble_wait_final_sig(ctx, final_sig, 120000u);
    bar_anim_stop(&sanim, sanim_h);
    if (!got_final) {
        jpp_sdk_ble_advertise_stop(ctx, &br);
        jpp_sdk_ble_service_unregister(ctx, &br);
        notify(ctx, "Timed out", "No final signature received.");
        return;
    }

    jpp_sdk_ble_advertise_stop(ctx, &br);
    jpp_sdk_ble_service_unregister(ctx, &br);

    /* Rebuild the proof from the leader's nicknames + timestamp (forwarded in
       round-1.5), in all_pubkeys order. Using the leader's values — not local
       "PeerN" placeholders or a fresh timestamp — makes the rebuilt message
       hash to exactly what was signed, so the saved proof verifies. */
    static meetapp_proof_participant_t participants[MEETAPP_MAX_TOTAL];
    for (size_t i = 0u; i < n_total; i++) {
        memcpy(participants[i].pubkey, &all_pubkeys[i * 32u], 32u);
        strncpy(participants[i].nickname, peer_nicknames[i], MEETAPP_NICKNAME_MAX);
        participants[i].nickname[MEETAPP_NICKNAME_MAX] = '\0';
    }

    bool saved = meetapp_proof_save(ctx, n_total, participants,
                                    session_nonce, leader_timestamp,
                                    leader_comment, final_sig);

    char ts_short[12];
    strncpy(ts_short, leader_timestamp, 11u);
    ts_short[11] = '\0';

    if (saved) {
        jpp_sdk_buzzer_play(ctx, JPP_BUZZER_SOUND_SUCCESS);
        char saved_msg[64];
        snprintf(saved_msg, sizeof(saved_msg), "Signed %s", ts_short);
        notify_ok(ctx, "Proof saved", saved_msg);
    } else {
        notify(ctx, "Save failed", "Proof signed but could not be saved.");
    }
}

/* ---- Entry point --------------------------------------------------------- */

void meetapp_run(jpp_sdk_context_t *ctx)
{
    ESP_LOGI(TAG, "meetapp_run started");

    /* Load or create identity (nickname + Ed25519 keypair). */
    static meetapp_identity_t identity;
    {
        const char *lp[2] = { "MeetApp", "Loading..." };
        set_text_frame(ctx, lp, 2u);
    }

    if (!meetapp_identity_load_or_create(ctx, &identity)) {
        jpp_sdk_ui_result_t r;
        jpp_sdk_dialog(ctx, "Identity error",
                       "Could not load or create your identity.", &r);
        jpp_sdk_request_close(ctx);
        return;
    }

    /* Front-load the persisted BLE grants (ble.scan + ble.advertise, tier-1) at
       startup: both participation modes need them, and a granted tier-1 cap is
       persisted, so asking once up front means the user is never interrupted for
       these mid-flow. Denial isn't fatal here — the per-mode flows re-surface it
       via their own guards — so we don't block entry to the menu. */
    jpp_sdk_request_cap(ctx, "ble.scan");
    jpp_sdk_request_cap(ctx, "ble.advertise");

    /* Main menu loop. Each action returns here; only backing out of the menu
       (long-press CENTER) exits the app. */
    static const char *const ITEMS[] = {
        "Join meetup",
        "Initiate meetup",
        "Reset identity",
        "Exit"
    };

    while (true) {
        int choice = menu_select(ctx, "MeetApp", ITEMS,
                                 sizeof(ITEMS) / sizeof(ITEMS[0]));
        if (choice < 0 || choice == 3) {
            break;                 /* back on the main screen exits the app */
        }
        switch (choice) {
        case 0:
            /* Join (participant): ask for the one-time ble.host grant (tier-2,
               session-scoped) the moment the mode is chosen, not deep in the
               flow at jpp_sdk_ble_service_register. Denial aborts before we start
               scanning. */
            if (jpp_sdk_request_cap(ctx, "ble.host") ==
                    JPP_SDK_STATUS_ACCESS_DENIED) {
                notify(ctx, "Permission needed", "Bluetooth access was denied.");
                break;
            }
            run_participant(ctx, &identity);
            break;
        case 1:
            /* Initiate (leader): ask for the one-time ble.connect grant (tier-2,
               session-scoped) up front. Without this the prompt would first fire
               inside the GATT collection loop, where a denial is silently
               swallowed. */
            if (jpp_sdk_request_cap(ctx, "ble.connect") ==
                    JPP_SDK_STATUS_ACCESS_DENIED) {
                notify(ctx, "Permission needed", "Bluetooth access was denied.");
                break;
            }
            run_leader(ctx, &identity);
            break;
        case 2:
            if (confirm_reset(ctx)) {
                if (meetapp_identity_reset(ctx, &identity)) {
                    notify_ok(ctx, "Identity reset",
                              "Your new identity is ready.");
                } else {
                    notify(ctx, "Reset cancelled",
                           "Your identity was not changed.");
                }
            }
            break;
        default:
            break;
        }
    }

    jpp_sdk_request_close(ctx);
    ESP_LOGI(TAG, "meetapp_run complete");
}
