#include "games_gfx.h"

#include <string.h>

/*
 * Shadow framebuffer: 64 rows × 16 bytes (MSB = leftmost pixel), same layout
 * as the SDK canvas. All drawing goes through games_gfx_px() so the optional
 * 90° rotation (Tetris) applies to every primitive, text included. flush()
 * writes only dirty rows and re-enters fullscreen mode when a modal UI helper
 * (dialog/list — they call jpp_sdk_set_frame) dropped it; the framebuffer is
 * the source of truth, so the full image survives any modal excursion.
 */

#define GFX_ROWS  64
#define GFX_COLS  128
#define GFX_ROW_BYTES JPP_SDK_CANVAS_ROW_BYTES

static jpp_sdk_context_t *s_ctx;
static uint8_t  s_fb[GFX_ROWS][GFX_ROW_BYTES];
static uint64_t s_dirty;
static bool     s_rot;

void games_gfx_init(jpp_sdk_context_t *ctx)
{
    s_ctx = ctx;
    s_rot = false;
    memset(s_fb, 0, sizeof(s_fb));
    s_dirty = ~0ull;
}

void games_gfx_set_rotation(bool rotated) { s_rot = rotated; }
int  games_gfx_width(void)  { return s_rot ? GFX_ROWS : GFX_COLS; }
int  games_gfx_height(void) { return s_rot ? GFX_COLS : GFX_ROWS; }

void games_gfx_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
    s_dirty = ~0ull;
}

void games_gfx_px(int x, int y, bool on)
{
    int px, py;
    if (s_rot) {
        /* Logical 64×128 for a device held sideways: logical x runs along the
           physical y axis (inverted), logical y along physical x. */
        px = y;
        py = (GFX_ROWS - 1) - x;
    } else {
        px = x;
        py = y;
    }
    if (px < 0 || px >= GFX_COLS || py < 0 || py >= GFX_ROWS) {
        return;
    }
    uint8_t *byte = &s_fb[py][px / 8];
    uint8_t  bit  = (uint8_t)(0x80u >> (px % 8));
    if (on) { *byte |= bit; } else { *byte &= (uint8_t)~bit; }
    s_dirty |= 1ull << py;
}

void games_gfx_hline(int x, int y, int w, bool on)
{
    for (int i = 0; i < w; i++) {
        games_gfx_px(x + i, y, on);
    }
}

void games_gfx_vline(int x, int y, int h, bool on)
{
    for (int i = 0; i < h; i++) {
        games_gfx_px(x, y + i, on);
    }
}

void games_gfx_rect(int x, int y, int w, int h, bool on)
{
    for (int j = 0; j < h; j++) {
        games_gfx_hline(x, y + j, w, on);
    }
}

void games_gfx_frame(int x, int y, int w, int h)
{
    games_gfx_hline(x, y, w, true);
    games_gfx_hline(x, y + h - 1, w, true);
    games_gfx_vline(x, y + 1, h - 2, true);
    games_gfx_vline(x + w - 1, y + 1, h - 2, true);
}

/* ---- 3×5 font -------------------------------------------------------------- */
/* One byte per row, low 3 bits used (bit2 = left pixel). */

static const uint8_t k_font_digits[10][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,2,2}, {7,5,7,5,7}, {7,5,7,1,7},
};

static const uint8_t k_font_upper[26][5] = {
    {2,5,7,5,5}, /* A */ {6,5,6,5,6}, /* B */ {3,4,4,4,3}, /* C */
    {6,5,5,5,6}, /* D */ {7,4,7,4,7}, /* E */ {7,4,7,4,4}, /* F */
    {3,4,5,5,3}, /* G */ {5,5,7,5,5}, /* H */ {7,2,2,2,7}, /* I */
    {1,1,1,5,2}, /* J */ {5,5,6,5,5}, /* K */ {4,4,4,4,7}, /* L */
    {5,7,7,5,5}, /* M */ {6,5,5,5,5}, /* N */ {7,5,5,5,7}, /* O */
    {7,5,7,4,4}, /* P */ {7,5,5,7,1}, /* Q */ {7,5,6,5,5}, /* R */
    {3,4,2,1,6}, /* S */ {7,2,2,2,2}, /* T */ {5,5,5,5,7}, /* U */
    {5,5,5,5,2}, /* V */ {5,5,7,7,5}, /* W */ {5,5,2,5,5}, /* X */
    {5,5,2,2,2}, /* Y */ {7,1,2,4,7}, /* Z */
};

static const uint8_t *glyph_rows(char c)
{
    static const uint8_t k_space[5]  = {0,0,0,0,0};
    static const uint8_t k_colon[5]  = {0,2,0,2,0};
    static const uint8_t k_dash[5]   = {0,0,7,0,0};
    static const uint8_t k_dot[5]    = {0,0,0,0,2};
    static const uint8_t k_bang[5]   = {2,2,2,0,2};
    static const uint8_t k_quest[5]  = {6,1,2,0,2};
    static const uint8_t k_gt[5]     = {4,2,1,2,4};
    static const uint8_t k_lt[5]     = {1,2,4,2,1};
    static const uint8_t k_slash[5]  = {1,1,2,4,4};
    static const uint8_t k_plus[5]   = {0,2,7,2,0};

    if (c >= '0' && c <= '9') return k_font_digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return k_font_upper[c - 'A'];
    if (c >= 'a' && c <= 'z') return k_font_upper[c - 'a'];
    switch (c) {
    case ':': return k_colon;
    case '-': return k_dash;
    case '.': return k_dot;
    case '!': return k_bang;
    case '?': return k_quest;
    case '>': return k_gt;
    case '<': return k_lt;
    case '/': return k_slash;
    case '+': return k_plus;
    default:  return k_space;
    }
}

void games_gfx_text(int x, int y, const char *s)
{
    for (; *s != '\0'; s++) {
        const uint8_t *rows = glyph_rows(*s);
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 3; c++) {
                if (rows[r] & (4u >> c)) {
                    games_gfx_px(x + c, y + r, true);
                }
            }
        }
        x += 4;
    }
}

int games_gfx_text_w(const char *s)
{
    int n = 0;
    while (s[n] != '\0') { n++; }
    return n > 0 ? n * 4 - 1 : 0;
}

/* ---- flush ------------------------------------------------------------------ */

void games_gfx_flush(void)
{
    if (s_ctx == NULL) {
        return;
    }
    /* A dialog/list helper exits fullscreen via set_frame and clears the SDK
       canvas; re-enter and push every row so the full image comes back. */
    if (!s_ctx->canvas_fullscreen) {
        jpp_sdk_canvas_fullscreen(s_ctx, true);
        s_dirty = ~0ull;
    }
    for (int row = 0; row < GFX_ROWS; row++) {
        if (s_dirty & (1ull << row)) {
            jpp_sdk_canvas_write(s_ctx, (uint8_t)row, s_fb[row]);
        }
    }
    s_dirty = 0u;
}
