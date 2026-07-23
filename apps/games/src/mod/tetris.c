/*
 * Tetris — Games-app module.
 *
 * The board logic is orientation-independent; rendering and the key map go
 * through the hub gfx transform. Default is the rotated layout (device held
 * sideways, 90° clockwise — right edge down): the playfield is 10×20 with
 * 5×5 px cells on a logical 64×128 screen. The "Tetris: Normal" setting
 * switches to an upright 128×64 layout with 3×3 px cells and a side panel.
 *
 * Sounds: rotate 1200 Hz blip · hard-drop 400 Hz thud · lock 250 Hz tick ·
 * line-clear fanfares that grow with the clear count · level-up arpeggio ·
 * descending game-over run.
 */

#include "games_api.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const games_api_t *api;
static jpp_sdk_context_t *s_ctx;

#define BW 10
#define BH 20
#define TICK_MS 33u

/* 4×4 piece masks, bit (r,c) = mask & (0x8000 >> (r*4+c)). I J L O S T Z. */
static const uint16_t k_pieces[7][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 },
    { 0x8E00, 0x6440, 0x0E20, 0x44C0 },
    { 0x2E00, 0x4460, 0x0E80, 0xC440 },
    { 0x6600, 0x6600, 0x6600, 0x6600 },
    { 0x6C00, 0x4620, 0x06C0, 0x8C40 },
    { 0x4E00, 0x4640, 0x0E40, 0x4C40 },
    { 0xC600, 0x2640, 0x0C60, 0x4C80 },
};

typedef struct {
    uint16_t board[BH];     /* bit c set = cell (row, c) filled */
    int type, rot, x, y;    /* falling piece */
    int next_type;
    uint8_t bag[7];
    int bag_pos;
    int score, lines, level;
    bool game_over;
    bool rotated;           /* layout */
} tetris_t;

static tetris_t g;

/* ---- bag + collision -------------------------------------------------------- */

static int bag_next(void)
{
    if (g.bag_pos >= 7) {
        for (int i = 0; i < 7; i++) { g.bag[i] = (uint8_t)i; }
        for (int i = 6; i > 0; i--) {
            int j = (int)api->rng((uint32_t)(i + 1));
            uint8_t t = g.bag[i]; g.bag[i] = g.bag[j]; g.bag[j] = t;
        }
        g.bag_pos = 0;
    }
    return g.bag[g.bag_pos++];
}

static bool collides(int type, int rot, int x, int y)
{
    uint16_t mask = k_pieces[type][rot];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!(mask & (0x8000u >> (r * 4 + c)))) { continue; }
            int bx = x + c;
            int by = y + r;
            if (bx < 0 || bx >= BW || by >= BH) { return true; }
            if (by >= 0 && (g.board[by] & (1u << bx))) { return true; }
        }
    }
    return false;
}

static void spawn(void)
{
    g.type = g.next_type;
    g.next_type = bag_next();
    g.rot = 0;
    g.x = 3;
    g.y = -1;
    if (collides(g.type, g.rot, g.x, g.y)) {
        g.game_over = true;
    }
}

/* ---- scoring + locking ------------------------------------------------------- */

static void sfx_line_clear(int n)
{
    static const jpp_buzzer_note_t k1[] = { {660,80},{880,80} };
    static const jpp_buzzer_note_t k2[] = { {660,60},{880,60},{1100,80} };
    static const jpp_buzzer_note_t k3[] = { {660,60},{880,60},{1100,60},{1320,100} };
    static const jpp_buzzer_note_t k4[] = { {880,70},{1100,70},{1320,70},
                                            {1760,150},{1320,70},{1760,200} };
    switch (n) {
    case 1: api->sfx_seq(k1, 2); break;
    case 2: api->sfx_seq(k2, 3); break;
    case 3: api->sfx_seq(k3, 4); break;
    default: api->sfx_seq(k4, 6); break;
    }
}

static void lock_piece(void)
{
    uint16_t mask = k_pieces[g.type][g.rot];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!(mask & (0x8000u >> (r * 4 + c)))) { continue; }
            int by = g.y + r;
            if (by >= 0 && by < BH) {
                g.board[by] |= (uint16_t)(1u << (g.x + c));
            }
        }
    }
    int cleared = 0;
    for (int row = BH - 1; row >= 0; row--) {
        if (g.board[row] == (uint16_t)((1u << BW) - 1u)) {
            cleared++;
            for (int r = row; r > 0; r--) { g.board[r] = g.board[r - 1]; }
            g.board[0] = 0u;
            row++;  /* re-check the row that slid down */
        }
    }
    if (cleared > 0) {
        static const int k_points[5] = { 0, 40, 100, 300, 1200 };
        g.score += k_points[cleared] * (g.level + 1);
        g.lines += cleared;
        int new_level = g.lines / 10;
        sfx_line_clear(cleared);
        if (new_level > g.level) {
            g.level = new_level;
            static const jpp_buzzer_note_t up[] =
                { {523,60},{659,60},{784,60},{1047,120} };
            api->sfx_seq(up, 4);
        }
    } else {
        api->sfx_tone(250u, 25u);
    }
    spawn();
}

/* ---- rendering ----------------------------------------------------------------- */
/* Rotated: cell 5 px, frame at (5,10), panel strip above and below.
   Normal:  cell 3 px, frame at (2,1), text panel on the right. */

static void draw_cell(int bx, int by, int cell, int ox, int oy, bool on)
{
    api->gfx_rect(ox + bx * cell, oy + by * cell, cell - 1, cell - 1, on);
}

static void draw_piece_at(int type, int rot, int bx, int by,
                          int cell, int ox, int oy)
{
    uint16_t mask = k_pieces[type][rot];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (mask & (0x8000u >> (r * 4 + c))) {
                draw_cell(bx + c, by + r, cell, ox, oy, true);
            }
        }
    }
}

static void render(void)
{
    int cell = g.rotated ? 5 : 3;
    int ox   = g.rotated ? 7 : 3;
    int oy   = g.rotated ? 11 : 2;
    char text[24];

    api->gfx_clear();
    api->gfx_frame(ox - 2, oy - 2, BW * cell + 3, BH * cell + 3);

    for (int r = 0; r < BH; r++) {
        for (int c = 0; c < BW; c++) {
            if (g.board[r] & (1u << c)) {
                draw_cell(c, r, cell, ox, oy, true);
            }
        }
    }
    if (!g.game_over) {
        draw_piece_at(g.type, g.rot, g.x, g.y, cell, ox, oy);
    }

    if (g.rotated) {
        /* score above the board, next preview below */
        snprintf(text, sizeof(text), "%d L%d", g.score, g.level);
        api->gfx_text(2, 2, text);
        api->gfx_text(2, 116, "NEXT");
        uint16_t mask = k_pieces[g.next_type][0];
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                if (mask & (0x8000u >> (r * 4 + c))) {
                    api->gfx_rect(24 + c * 3, 114 + r * 3, 2, 2, true);
                }
            }
        }
    } else {
        int px = 44;
        api->gfx_text(px, 2, "SCORE");
        snprintf(text, sizeof(text), "%d", g.score);
        api->gfx_text(px, 9, text);
        api->gfx_text(px, 19, "LEVEL");
        snprintf(text, sizeof(text), "%d", g.level);
        api->gfx_text(px, 26, text);
        api->gfx_text(px, 36, "LINES");
        snprintf(text, sizeof(text), "%d", g.lines);
        api->gfx_text(px, 43, text);
        api->gfx_text(px + 34, 2, "NEXT");
        uint16_t mask = k_pieces[g.next_type][0];
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                if (mask & (0x8000u >> (r * 4 + c))) {
                    api->gfx_rect(px + 34 + c * 4, 10 + r * 4, 3, 3, true);
                }
            }
        }
    }
    api->gfx_flush();
}

/* ---- input -------------------------------------------------------------------- */

static void try_move(int dx)
{
    if (!collides(g.type, g.rot, g.x + dx, g.y)) {
        g.x += dx;
    }
}

static void try_rotate(void)
{
    int rot = (g.rot + 1) % 4;
    /* simple wall kicks: try in place, then one cell left/right */
    static const int k_kicks[3] = { 0, -1, 1 };
    for (int i = 0; i < 3; i++) {
        if (!collides(g.type, rot, g.x + k_kicks[i], g.y)) {
            g.rot = rot;
            g.x += k_kicks[i];
            api->sfx_tone(1200u, 20u);
            return;
        }
    }
}

static void soft_drop(void)
{
    if (!collides(g.type, g.rot, g.x, g.y + 1)) {
        g.y++;
        g.score += 1;
    } else {
        lock_piece();
    }
}

static void hard_drop(void)
{
    while (!collides(g.type, g.rot, g.x, g.y + 1)) {
        g.y++;
        g.score += 2;
    }
    api->sfx_tone(400u, 30u);
    lock_piece();
}

static int save_score(void)
{
    int best = api->store_get(GAMES_KV_TETRIS_HS, 0);
    if (g.score > best) {
        best = g.score;
        api->store_set(GAMES_KV_TETRIS_HS, best);
    }
    return best;
}

/* returns 0 = resume, 1 = restart, 2 = quit */
static int pause_menu(void)
{
    static const char *items[] = { "Resume", "Restart", "Quit" };
    size_t index = 0u, count = 0u;
    jpp_sdk_ui_result_t ui;
    if (jpp_sdk_list(s_ctx, "Paused", items, 3u, false,
                     &index, 1u, &count, &ui) != JPP_SDK_STATUS_OK ||
        ui == JPP_SDK_UI_BACK || count == 0u) {
        return 0;
    }
    if ((int)index == 2) { save_score(); }
    return (int)index;
}

/* Uniform control scheme in both orientations: CENTER rotates, UP hard-drops,
   DOWN soft-drops, LEFT/RIGHT move. (In the rotated layout the physical d-pad
   turns with the device; the lateral keys still move the piece across the
   board, the down keys still drive it toward the floor.) */
static int handle_key(jpp_sdk_key_event_t key)
{
    if (key == JPP_SDK_KEY_CENTER_LONG) {
        return pause_menu();
    }
    if (g.rotated) {
        switch (key) {
        case JPP_SDK_KEY_UP:    try_move(1);  break;
        case JPP_SDK_KEY_DOWN:  try_move(-1); break;
        case JPP_SDK_KEY_LEFT:  hard_drop(); break;
        case JPP_SDK_KEY_RIGHT: soft_drop();  break;
        case JPP_SDK_KEY_CENTER: try_rotate(); break;
        default: break;
        }
    } else {
        switch (key) {
        case JPP_SDK_KEY_LEFT:  try_move(-1); break;
        case JPP_SDK_KEY_RIGHT: try_move(1);  break;
        case JPP_SDK_KEY_UP:    try_rotate(); break;
        case JPP_SDK_KEY_DOWN:  soft_drop();  break;
        case JPP_SDK_KEY_CENTER: try_rotate(); break;
        default: break;
        }
    }
    return 0;
}

/* ---- game session --------------------------------------------------------------- */

static void reset_game(void)
{
    tetris_t fresh = { 0 };
    fresh.rotated = g.rotated;
    g = fresh;
    g.bag_pos = 7;
    g.next_type = bag_next();
    spawn();
}

/* returns true to play again */
static bool play_session(void)
{
    uint32_t fall_acc = 0u;

    reset_game();
    while (!g.game_over) {
        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        while (jpp_sdk_poll_key(s_ctx, &key) == JPP_SDK_STATUS_OK &&
               key != JPP_SDK_KEY_NONE) {
            int action = handle_key(key);
            if (action == 1) { reset_game(); fall_acc = 0u; }
            if (action == 2) { return false; }
            if (g.game_over) { break; }
            key = JPP_SDK_KEY_NONE;
        }
        if (g.game_over) { break; }

        uint32_t interval = 800u > 70u * (uint32_t)g.level
                          ? 800u - 70u * (uint32_t)g.level : 120u;
        if (interval < 120u) { interval = 120u; }
        fall_acc += TICK_MS;
        if (fall_acc >= interval) {
            fall_acc = 0u;
            if (!collides(g.type, g.rot, g.x, g.y + 1)) {
                g.y++;
            } else {
                lock_piece();
            }
        }
        render();
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }

    static const jpp_buzzer_note_t k_over[] =
        { {784,120},{659,120},{523,120},{392,250} };
    api->sfx_seq(k_over, 4);

    int best = save_score();
    char text[48];
    snprintf(text, sizeof(text), "Score %d  Best %d. OK = play again.",
             g.score, best);
    jpp_sdk_ui_result_t ui;
    jpp_sdk_dialog(s_ctx, "Game over", text, &ui);
    return ui == JPP_SDK_UI_OK;
}

void jpp_module_entry(jpp_sdk_context_t *ctx, void *api_ptr)
{
    api = (const games_api_t *)api_ptr;
    if (api == NULL || api->version != GAMES_API_VERSION) {
        return;
    }
    s_ctx = ctx;

    g.rotated = api->store_get(GAMES_KV_TETRIS_ROT, 1) != 0;
    api->gfx_set_rotation(g.rotated);

    while (play_session()) { }

    api->gfx_set_rotation(false);
}
