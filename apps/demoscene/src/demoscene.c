/*
 * DemoScene — an oldskool megademo for the J++Device.
 *
 * Eight scenes on the fullscreen 128×64 1-bit canvas — title card, 3D
 * starfield, plasma, rotozoomer, tunnel, wireframe cube, doom fire and a
 * credits screen with raster bars — plus a sine-wave scrolltext overlay and
 * a looping square-wave chiptune on the buzzer. Everything is integer math
 * (the C6 has no FPU): a 256-step sine LUT drives the motion and a 4×4
 * Bayer matrix fakes greyscale by ordered dithering.
 *
 * Controls: LEFT/RIGHT switch scene, UP toggles music, DOWN toggles
 * auto-advance, long-press CENTER exits.
 */

#include "demoscene.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DW 128
#define DH 64
#define ROWB JPP_SDK_CANVAS_ROW_BYTES

#define FRAME_MS      33u     /* ~30 fps */
#define SCENE_MS      12000u  /* auto-advance period */
#define OSD_FRAMES    45u     /* scene-name overlay after a manual switch */

static jpp_sdk_context_t *s_ctx;
static uint8_t s_fb[DH][ROWB];

/* ---- Integer trig / dither / rng ------------------------------------------ */

/* Quarter sine wave, 64 steps, amplitude 127. */
static const uint8_t k_qsin[64] = {
      0,   3,   6,   9,  12,  16,  19,  22,  25,  28,  31,  34,  37,  40,  43,  46,
     49,  51,  54,  57,  60,  63,  65,  68,  71,  73,  76,  78,  81,  83,  85,  88,
     90,  92,  94,  96,  98, 100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116,
    117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127,
};

/* sin over a 0–255 circle, returns -127..127. */
static int sin8(uint8_t a)
{
    uint8_t i = a & 63u;
    switch (a >> 6) {
    case 0:  return  k_qsin[i];
    case 1:  return  k_qsin[63u - i];
    case 2:  return -k_qsin[i];
    default: return -k_qsin[63u - i];
    }
}

static int cos8(uint8_t a) { return sin8((uint8_t)(a + 64u)); }

/* 4×4 Bayer ordered-dither matrix (thresholds 0–15). */
static const uint8_t k_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static uint32_t s_rng_state = 0x2A5C1D3Bu;

static uint32_t rng(void)
{
    uint32_t x = s_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng_state = x;
    return x;
}

static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0u, bit = 1u << 30;
    while (bit > v) { bit >>= 2; }
    while (bit != 0u) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* atan2 over a 0–255 circle (octant approximation, good enough for texture). */
static uint8_t iatan2_8(int y, int x)
{
    if (x == 0 && y == 0) {
        return 0u;
    }
    int ax = x < 0 ? -x : x;
    int ay = y < 0 ? -y : y;
    int a;  /* 0..64 = first quadrant */
    if (ax > ay) {
        a = (ay * 32) / ax;
    } else {
        a = 64 - (ax * 32) / ay;
    }
    if (x < 0) { a = 128 - a; }
    if (y < 0) { a = 256 - a; }
    return (uint8_t)a;
}

/* ---- Framebuffer primitives ------------------------------------------------ */

static inline void px(int x, int y, bool on)
{
    if ((unsigned)x >= DW || (unsigned)y >= DH) {
        return;
    }
    uint8_t *byte = &s_fb[y][x >> 3];
    uint8_t  bit  = (uint8_t)(0x80u >> (x & 7));
    if (on) { *byte |= bit; } else { *byte &= (uint8_t)~bit; }
}

static void fb_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

static void line(int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px(x0, y0, true);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Paint one 2×2 block at block coords (bx, by) with intensity v (0–255),
   ordered-dithered — the workhorse for the half-resolution effects. */
static void block2(int bx, int by, uint8_t v)
{
    int x = bx * 2, y = by * 2;
    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            uint8_t thr = (uint8_t)(k_bayer[(y + dy) & 3][(x + dx) & 3] * 16u + 8u);
            px(x + dx, y + dy, v > thr);
        }
    }
}

/* ---- 3×5 font (same glyph data as the Games app) --------------------------- */

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
    static const uint8_t k_space[5] = {0,0,0,0,0};
    static const uint8_t k_dash[5]  = {0,0,7,0,0};
    static const uint8_t k_dot[5]   = {0,0,0,0,2};
    static const uint8_t k_bang[5]  = {2,2,2,0,2};
    static const uint8_t k_star[5]  = {5,2,7,2,5};
    static const uint8_t k_plus[5]  = {0,2,7,2,0};
    static const uint8_t k_colon[5] = {0,2,0,2,0};
    static const uint8_t k_slash[5] = {1,1,2,4,4};

    if (c >= '0' && c <= '9') return k_font_digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return k_font_upper[c - 'A'];
    if (c >= 'a' && c <= 'z') return k_font_upper[c - 'a'];
    switch (c) {
    case '-': return k_dash;
    case '.': return k_dot;
    case '!': return k_bang;
    case '*': return k_star;
    case '+': return k_plus;
    case ':': return k_colon;
    case '/': return k_slash;
    default:  return k_space;
    }
}

/* Draw text scaled up by an integer factor (1 = 3×5, 2 = 6×10, 3 = 9×15). */
static void text(int x, int y, const char *s, int scale)
{
    for (; *s != '\0'; s++) {
        const uint8_t *rows = glyph_rows(*s);
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 3; c++) {
                if (rows[r] & (4u >> c)) {
                    for (int j = 0; j < scale; j++) {
                        for (int i = 0; i < scale; i++) {
                            px(x + c * scale + i, y + r * scale + j, true);
                        }
                    }
                }
            }
        }
        x += 4 * scale;
    }
}

static int text_w(const char *s, int scale)
{
    int n = 0;
    while (s[n] != '\0') { n++; }
    return n > 0 ? n * 4 * scale - scale : 0;
}

/* Text with a 1px cleared halo so it stays readable over a busy effect. */
static void text_halo(int x, int y, const char *s, int scale)
{
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            /* Clear pass: stamp the glyph shape as "off" around the target. */
            const char *p = s;
            int cx = x + dx;
            for (; *p != '\0'; p++) {
                const uint8_t *rows = glyph_rows(*p);
                for (int r = 0; r < 5; r++) {
                    for (int c = 0; c < 3; c++) {
                        if (rows[r] & (4u >> c)) {
                            for (int j = 0; j < scale; j++) {
                                for (int i = 0; i < scale; i++) {
                                    px(cx + c * scale + i, y + dy + r * scale + j, false);
                                }
                            }
                        }
                    }
                }
                cx += 4 * scale;
            }
        }
    }
    text(x, y, s, scale);
}

/* ---- Per-scene state (one effect at a time, so a union keeps .bss small) --- */

#define STAR_COUNT 96

static union {
    struct {
        int16_t  x[STAR_COUNT];
        int16_t  y[STAR_COUNT];
        uint16_t z[STAR_COUNT];
    } stars;
    struct {
        uint8_t ang[32][64];   /* texture angle per 2×2 block */
        uint8_t dep[32][64];   /* texture depth per 2×2 block */
    } tun;
    struct {
        uint8_t heat[34][64];  /* 32 visible block rows + 2 seed rows */
    } fire;
} s_fx;

/* ---- Scene: title card ------------------------------------------------------ */

static void scene_intro(uint32_t t)
{
    fb_clear();
    /* Twinkling static stars from a cheap hash. */
    for (int i = 0; i < 40; i++) {
        uint32_t h = (uint32_t)i * 2654435761u;
        int sx = (int)(h % DW);
        int sy = (int)((h >> 8) % DH);
        if (((t >> 3) + i) % 7 != 0) {
            px(sx, sy, true);
        }
    }
    text_halo((DW - text_w("FABLE 5 PRESENTS", 1)) / 2, 6, "FABLE 5 PRESENTS", 1);
    text_halo((DW - text_w("JPPDOS", 3)) / 2, 18, "JPPDOS", 3);
    text_halo((DW - text_w("MEGADEMO", 2)) / 2, 38, "MEGADEMO", 2);
    /* Bottom line blinks, alternating between the tagline and the prompt. */
    switch ((t >> 4) & 3u) {
    case 0:
        text_halo((DW - text_w("PRESS OK TO PROCEED", 1)) / 2, 56,
                  "PRESS OK TO PROCEED", 1);
        break;
    case 2:
        text_halo((DW - text_w("1 BIT - NO FPU - NO FEAR", 1)) / 2, 56,
                  "1 BIT - NO FPU - NO FEAR", 1);
        break;
    default:
        break;
    }
}

/* ---- Scene: 3D starfield ----------------------------------------------------- */

static void starfield_init(void)
{
    for (int i = 0; i < STAR_COUNT; i++) {
        s_fx.stars.x[i] = (int16_t)(rng() & 0x7FFFu) - 16384;
        s_fx.stars.y[i] = (int16_t)(rng() & 0x7FFFu) - 16384;
        s_fx.stars.z[i] = (uint16_t)(64u + (rng() % 4032u));
    }
}

static void scene_starfield(uint32_t t)
{
    fb_clear();
    /* Warp speed swells with a slow sine. */
    int speed = 48 + (sin8((uint8_t)(t / 2)) + 127) / 4;
    for (int i = 0; i < STAR_COUNT; i++) {
        int z = s_fx.stars.z[i] - speed;
        if (z < 32) {
            s_fx.stars.x[i] = (int16_t)(rng() & 0x7FFFu) - 16384;
            s_fx.stars.y[i] = (int16_t)(rng() & 0x7FFFu) - 16384;
            z = 4095;
        }
        s_fx.stars.z[i] = (uint16_t)z;
        int sx = 64 + (s_fx.stars.x[i] / z) * 2;
        int sy = 32 + (s_fx.stars.y[i] / z) * 2;
        if ((unsigned)sx >= DW || (unsigned)sy >= DH) {
            continue;
        }
        px(sx, sy, true);
        if (z < 1024) {  /* near stars get bigger */
            px(sx + 1, sy, true);
            if (z < 400) {
                px(sx, sy + 1, true);
                px(sx + 1, sy + 1, true);
            }
        }
    }
}

/* ---- Scene: plasma ------------------------------------------------------------ */

static void scene_plasma(uint32_t t)
{
    uint8_t t2 = (uint8_t)(t * 2), t3 = (uint8_t)(t * 3), t4 = (uint8_t)(t * 4);
    int cx = 32 + sin8((uint8_t)(t * 2)) / 8;   /* drifting radial center */
    int cy = 16 + cos8((uint8_t)(t * 3)) / 12;
    for (int by = 0; by < 32; by++) {
        int dy = by - cy;
        for (int bx = 0; bx < 64; bx++) {
            int dx = bx - cx;
            int d = (int)isqrt32((uint32_t)(dx * dx + dy * dy));
            int v = sin8((uint8_t)(bx * 6 + t2))
                  + sin8((uint8_t)(by * 9 - t3))
                  + sin8((uint8_t)((bx + by) * 4 + t4))
                  + sin8((uint8_t)(d * 9 - t3));
            block2(bx, by, (uint8_t)((v + 512) >> 2));
        }
    }
}

/* ---- Scene: rotozoomer --------------------------------------------------------- */

static void scene_rotozoom(uint32_t t)
{
    /* 8.8 fixed point; texture = 8px checkerboard indexed by u,v. */
    int zoom = 300 + sin8((uint8_t)(t * 2)) * 3 / 2;   /* 110..490 */
    int c = cos8((uint8_t)t) * zoom / 128;
    int s = sin8((uint8_t)t) * zoom / 128;
    int ox = (int)(t * 97);   /* slow drift through the texture */
    int oy = (int)(t * 31);
    for (int y = 0; y < DH; y++) {
        int u = ox + (-64) * c - (y - 32) * s;
        int v = oy + (-64) * s + (y - 32) * c;
        for (int x = 0; x < DW; x++) {
            px(x, y, (((u >> 11) ^ (v >> 11)) & 1) != 0);
            u += c;
            v += s;
        }
    }
}

/* ---- Scene: tunnel --------------------------------------------------------------- */

static void tunnel_init(void)
{
    for (int by = 0; by < 32; by++) {
        int dy = by * 2 - 31;
        for (int bx = 0; bx < 64; bx++) {
            int dx = bx * 2 - 63;
            uint32_t r = isqrt32((uint32_t)(dx * dx + dy * dy));
            if (r == 0u) {
                r = 1u;
            }
            uint32_t d = 2048u / r;
            s_fx.tun.ang[by][bx] = iatan2_8(dy, dx);
            s_fx.tun.dep[by][bx] = (uint8_t)(d > 255u ? 255u : d);
        }
    }
}

static void scene_tunnel(uint32_t t)
{
    uint8_t rot  = (uint8_t)(t * 2 + sin8((uint8_t)t) / 4);  /* swaying rotation */
    uint8_t fly  = (uint8_t)(t * 6);
    for (int by = 0; by < 32; by++) {
        for (int bx = 0; bx < 64; bx++) {
            uint8_t dep = s_fx.tun.dep[by][bx];
            bool on = ((((uint8_t)(s_fx.tun.ang[by][bx] + rot) >> 5)
                      ^ ((uint8_t)(dep + fly) >> 5)) & 1u) != 0u;
            /* Fade to black toward the far center (high dep = far). */
            uint8_t bright = (uint8_t)(255u - (dep > 220u ? 220u : dep));
            block2(bx, by, on ? bright : 0u);
        }
    }
}

/* ---- Scene: wireframe cube --------------------------------------------------------- */

static const int8_t k_cube_v[8][3] = {
    {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
    {-1,-1, 1}, {1,-1, 1}, {1,1, 1}, {-1,1, 1},
};

static const uint8_t k_cube_e[12][2] = {
    {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7},
};

static void scene_cube(uint32_t t)
{
    fb_clear();
    int size = 52 + sin8((uint8_t)(t * 4)) / 8;  /* pulse with the beat */
    int sx1 = sin8((uint8_t)(t * 2)), cx1 = cos8((uint8_t)(t * 2));
    int sy1 = sin8((uint8_t)(t * 3)), cy1 = cos8((uint8_t)(t * 3));
    int sz1 = sin8((uint8_t)t),       cz1 = cos8((uint8_t)t);
    int pxs[8], pys[8];
    for (int i = 0; i < 8; i++) {
        int x = k_cube_v[i][0] * size;
        int y = k_cube_v[i][1] * size;
        int z = k_cube_v[i][2] * size;
        /* rotate X */
        int y1 = (y * cx1 - z * sx1) >> 7;
        int z1 = (y * sx1 + z * cx1) >> 7;
        /* rotate Y */
        int x2 = (x * cy1 + z1 * sy1) >> 7;
        int z2 = (-x * sy1 + z1 * cy1) >> 7;
        /* rotate Z */
        int x3 = (x2 * cz1 - y1 * sz1) >> 7;
        int y3 = (x2 * sz1 + y1 * cz1) >> 7;
        int d = z2 + 260;
        pxs[i] = 64 + (x3 * 70) / d;
        pys[i] = 32 + (y3 * 70) / d;
    }
    for (int i = 0; i < 12; i++) {
        line(pxs[k_cube_e[i][0]], pys[k_cube_e[i][0]],
             pxs[k_cube_e[i][1]], pys[k_cube_e[i][1]]);
    }
}

/* ---- Scene: doom fire ------------------------------------------------------------------ */

static void fire_init(void)
{
    memset(&s_fx.fire, 0, sizeof(s_fx.fire));
}

static void scene_fire(uint32_t t)
{
    /* Reseed the two hidden rows below the screen with hot/cold patches. */
    for (int x = 0; x < 64; x++) {
        uint8_t v = (rng() & 7u) > 1u ? 255u : 0u;
        s_fx.fire.heat[32][x] = v;
        s_fx.fire.heat[33][x] = v;
    }
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 64; x++) {
            int xl = x > 0 ? x - 1 : 0;
            int xr = x < 63 ? x + 1 : 63;
            int v = (s_fx.fire.heat[y + 1][xl] + s_fx.fire.heat[y + 1][x]
                   + s_fx.fire.heat[y + 1][xr] + s_fx.fire.heat[y + 2][x]) >> 2;
            v -= (int)(rng() & 15u);
            s_fx.fire.heat[y][x] = (uint8_t)(v < 0 ? 0 : v);
            block2(x, y, s_fx.fire.heat[y][x]);
        }
    }
}

/* ---- Scene: credits + raster bars -------------------------------------------------------- */

static void scene_credits(uint32_t t)
{
    /* Three sine-bouncing horizontal raster bars, dithered bright at the core. */
    fb_clear();
    for (int k = 0; k < 3; k++) {
        int yc = 32 + sin8((uint8_t)(t * 3 + k * 85)) / 3;  /* -42..42 around center */
        for (int dy = -5; dy <= 5; dy++) {
            int y = yc + dy;
            if ((unsigned)y >= DH) {
                continue;
            }
            uint8_t v = (uint8_t)(200 - abs(dy) * 36);
            for (int x = 0; x < DW; x++) {
                uint8_t thr = (uint8_t)(k_bayer[y & 3][x & 3] * 16u + 8u);
                if (v > thr) {
                    px(x, y, true);
                }
            }
        }
    }
    text_halo((DW - text_w("CODE + GFX + MUSIC", 1)) / 2,  8, "CODE + GFX + MUSIC", 1);
    text_halo((DW - text_w("FABLE 5", 2)) / 2,            17, "FABLE 5", 2);
    text_halo((DW - text_w("GREETZ:", 1)) / 2,            34, "GREETZ:", 1);
    text_halo((DW - text_w("M4L3VICH * J++ CHAT", 1)) / 2, 42,
              "M4L3VICH * J++ CHAT", 1);
    text_halo((DW - text_w("LOOPS FOREVER...", 1)) / 2,   54, "LOOPS FOREVER...", 1);
}

/* ---- Scene table ----------------------------------------------------------------------- */

typedef struct {
    const char *name;
    void (*init)(void);           /* may be NULL */
    void (*render)(uint32_t t);   /* t = frames since scene start */
    bool scroller;                /* draw the scrolltext overlay */
} scene_t;

static const scene_t k_scenes[] = {
    { "INTRO",     NULL,           scene_intro,     false },
    { "STARFIELD", starfield_init, scene_starfield, true  },
    { "PLASMA",    NULL,           scene_plasma,    true  },
    { "ROTOZOOM",  NULL,           scene_rotozoom,  true  },
    { "TUNNEL",    tunnel_init,    scene_tunnel,    true  },
    { "CUBE",      NULL,           scene_cube,      true  },
    { "FIRE",      fire_init,      scene_fire,      true  },
    { "CREDITS",   NULL,           scene_credits,   false },
};
#define SCENE_COUNT (sizeof(k_scenes) / sizeof(k_scenes[0]))

/* ---- Scrolltext overlay ------------------------------------------------------------------- */

static const char k_scroll_msg[] =
    "* * *   JPPDOS MEGADEMO   * * *   "
    "RUNNING ON A 160 MHZ RISC-V WITH 128X64 PIXELS OF PURE OLDSKOOL...   "
    "ALL INTEGER MATH - THE BAYER MATRIX IS DOING THE HEAVY LIFTING...   "
    "LEFT/RIGHT: CHANGE SCENE   UP: MUSIC   DOWN: AUTOPILOT   "
    "HOLD CENTER TO EXIT...   "
    "GREETINGS FLY OUT TO EVERY DEVICE STUCK IN DUMMY MODE...   "
    "KEEP THE SCENE ALIVE!         ";

#define SCROLL_CHARS   ((int)sizeof(k_scroll_msg) - 1)
#define SCROLL_ADVANCE 8   /* px per char at scale 2 */

static void draw_scroller(uint32_t gt)
{
    int span = SCROLL_CHARS * SCROLL_ADVANCE + DW;
    int head = DW - (int)((gt * 2u) % (uint32_t)span);
    for (int i = 0; i < SCROLL_CHARS; i++) {
        int cx = head + i * SCROLL_ADVANCE;
        if (cx <= -SCROLL_ADVANCE || cx >= DW) {
            continue;
        }
        char buf[2] = { k_scroll_msg[i], '\0' };
        if (buf[0] == ' ') {
            continue;
        }
        int y = 48 + sin8((uint8_t)(cx * 2 + gt * 5)) * 5 / 127;
        text_halo(cx, y, buf, 2);
    }
}

/* ---- Chiptune engine ----------------------------------------------------------------------- */
/*
 * A tiny tracker: four bars (Am F C G), 16 sixteenth-steps per bar at 150 BPM,
 * each step an 82 ms tone + 18 ms rest. One bar (32 buzzer notes) is submitted
 * to the async player at a time; the main loop re-arms at each bar boundary.
 */

#define MUSIC_STEP_MS 100u
#define MUSIC_BAR_MS  (16u * MUSIC_STEP_MS)

/* [chord][voice]: bass, arp low, arp mid, arp high (Hz). */
static const uint16_t k_chords[4][4] = {
    { 110, 220, 262, 330 },   /* Am */
    {  87, 175, 220, 262 },   /* F  */
    { 131, 262, 330, 392 },   /* C  */
    {  98, 196, 247, 294 },   /* G  */
};

static const uint8_t k_steps[16]     = { 0,1,2,3, 0,2,1,3, 0,1,2,3, 0,3,2,1 };
static const uint8_t k_steps_alt[16] = { 0,3,2,1, 0,2,3,1, 0,3,2,1, 3,2,1,0 };

static void music_submit_bar(uint32_t bar)
{
    const uint16_t *chord = k_chords[bar & 3u];
    const uint8_t  *steps = ((bar >> 2) & 1u) ? k_steps_alt : k_steps;
    jpp_buzzer_note_t notes[32];
    for (int i = 0; i < 16; i++) {
        notes[2 * i].freq_hz      = chord[steps[i]];
        notes[2 * i].duration_ms  = 82u;
        notes[2 * i + 1].freq_hz     = 0u;
        notes[2 * i + 1].duration_ms = 18u;
    }
    jpp_sdk_buzzer_play_sequence_async(s_ctx, notes, 32u);
}

/* ---- Flush ------------------------------------------------------------------------------------ */

static void flush(void)
{
    if (!s_ctx->canvas_fullscreen) {
        jpp_sdk_canvas_fullscreen(s_ctx, true);
    }
    for (uint8_t row = 0u; row < DH; row++) {
        jpp_sdk_canvas_write(s_ctx, row, s_fb[row]);
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ---- Main loop --------------------------------------------------------------------------------- */

void demoscene_run(jpp_sdk_context_t *ctx)
{
    s_ctx = ctx;
    s_rng_state ^= now_ms() | 1u;

    jpp_sdk_wakelock_acquire(ctx);
    jpp_sdk_canvas_fullscreen(ctx, true);

    size_t   scene = 0u;
    uint32_t scene_t = 0u;          /* frames since scene start */
    uint32_t gt = 0u;               /* global frame counter */
    uint32_t scene_started = now_ms();
    uint32_t osd_until = 0u;        /* frame count; scene-name OSD after manual switch */
    bool     autopilot = true;
    bool     music_on = true;
    uint32_t music_bar = 0u;
    uint32_t music_next = now_ms();

    for (;;) {
        if (ctx->close_requested) {
            break;
        }

        /* Input — drain the queue so buffered presses don't skip scenes. */
        bool exit_requested = false;
        int  step = 0;
        jpp_sdk_key_event_t key;
        while (jpp_sdk_poll_key(ctx, &key) == JPP_SDK_STATUS_OK &&
               key != JPP_SDK_KEY_NONE) {
            switch (key) {
            case JPP_SDK_KEY_LEFT:  step -= 1; break;
            case JPP_SDK_KEY_RIGHT:
            case JPP_SDK_KEY_CENTER: step += 1; break;
            case JPP_SDK_KEY_UP:
                music_on = !music_on;
                if (music_on) {
                    music_next = now_ms();
                } else {
                    jpp_sdk_buzzer_stop(ctx);
                }
                break;
            case JPP_SDK_KEY_DOWN:
                autopilot = !autopilot;
                break;
            case JPP_SDK_KEY_CENTER_LONG:
                exit_requested = true;
                break;
            default:
                break;
            }
        }
        if (exit_requested) {
            break;
        }

        uint32_t now = now_ms();
        if (step == 0 && autopilot && now - scene_started >= SCENE_MS) {
            step = 1;
        }
        if (step != 0) {
            int next = ((int)scene + step) % (int)SCENE_COUNT;
            if (next < 0) {
                next += (int)SCENE_COUNT;
            }
            scene = (size_t)next;
            scene_t = 0u;
            scene_started = now;
            osd_until = gt + OSD_FRAMES;
            if (k_scenes[scene].init != NULL) {
                k_scenes[scene].init();
            }
        }

        /* Music — re-arm the async player at every bar boundary. */
        if (music_on && (int32_t)(now - music_next) >= 0) {
            music_submit_bar(music_bar++);
            music_next = now + MUSIC_BAR_MS;
        }

        k_scenes[scene].render(scene_t);
        if (k_scenes[scene].scroller) {
            draw_scroller(gt);
        }
        if (gt < osd_until) {
            text_halo(2, 2, k_scenes[scene].name, 1);
            if (!autopilot) {
                text_halo(2, 9, "MANUAL", 1);
            }
        }
        flush();

        gt++;
        scene_t++;
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }

    jpp_sdk_buzzer_stop(ctx);
    jpp_sdk_wakelock_release(ctx);
}
