/*
 * 2048 — Games-app module.
 *
 * 4×4 grid of exponents (tile value = 2^n) with 14×14 px tiles and a side
 * panel for score/best. D-pad slides; merged tiles add their value to the
 * score. Reaching 2048 shows a win banner once and lets the run continue.
 *
 * Sounds: soft 200 Hz slide tick · merge pitch rises with the tile rank ·
 * two-note flourish on a new personal-best tile · 2048 fanfare ·
 * descending game-over run.
 */

#include "games_api.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const games_api_t *api;
static jpp_sdk_context_t *s_ctx;

#define N 4
#define TILE 15              /* 14 px tile + 1 px gap */
#define GRID_X 2
#define GRID_Y 2

typedef struct {
    uint8_t cell[N][N];      /* exponent; 0 = empty */
    int score;
    int max_exp;
    bool won_shown;
    bool game_over;
} g2048_t;

static g2048_t g;

static void spawn_tile(void)
{
    uint8_t free_cells[N * N];
    int free_count = 0;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            if (g.cell[r][c] == 0u) {
                free_cells[free_count++] = (uint8_t)(r * N + c);
            }
        }
    }
    if (free_count == 0) {
        return;
    }
    uint8_t pick = free_cells[api->rng((uint32_t)free_count)];
    g.cell[pick / N][pick % N] = api->rng(10u) == 0u ? 2u : 1u;  /* 4 : 2 */
}

static bool any_moves(void)
{
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            if (g.cell[r][c] == 0u) { return true; }
            if (c + 1 < N && g.cell[r][c] == g.cell[r][c + 1]) { return true; }
            if (r + 1 < N && g.cell[r][c] == g.cell[r + 1][c]) { return true; }
        }
    }
    return false;
}

/* Slide and merge one line of 4 exponents toward index 0. Returns true if
   anything moved; accumulates score and tracks the best exponent. */
static bool slide_line(uint8_t line[N])
{
    uint8_t out[N] = { 0 };
    int pos = 0;
    bool moved = false;
    bool merged_here = false;

    for (int i = 0; i < N; i++) {
        if (line[i] == 0u) { continue; }
        if (pos > 0 && !merged_here && out[pos - 1] == line[i]) {
            out[pos - 1]++;
            g.score += 1 << out[pos - 1];
            if (out[pos - 1] > g.max_exp) { g.max_exp = out[pos - 1]; }
            api->sfx_tone(440u + (uint32_t)out[pos - 1] * 60u, 20u);
            merged_here = true;
            moved = true;
        } else {
            if (i != pos) { moved = true; }
            out[pos++] = line[i];
            merged_here = false;
        }
    }
    memcpy(line, out, N);
    return moved;
}

/* dir: 0 left, 1 right, 2 up, 3 down */
static bool slide(int dir)
{
    bool moved = false;
    for (int i = 0; i < N; i++) {
        uint8_t line[N];
        for (int j = 0; j < N; j++) {
            switch (dir) {
            case 0: line[j] = g.cell[i][j];         break;
            case 1: line[j] = g.cell[i][N - 1 - j]; break;
            case 2: line[j] = g.cell[j][i];         break;
            default: line[j] = g.cell[N - 1 - j][i]; break;
            }
        }
        if (slide_line(line)) { moved = true; }
        for (int j = 0; j < N; j++) {
            switch (dir) {
            case 0: g.cell[i][j] = line[j];         break;
            case 1: g.cell[i][N - 1 - j] = line[j]; break;
            case 2: g.cell[j][i] = line[j];         break;
            default: g.cell[N - 1 - j][i] = line[j]; break;
            }
        }
    }
    return moved;
}

static void tile_label(uint8_t exp, char *out, size_t out_len)
{
    unsigned value = 1u << exp;
    if (value >= 1024u) {
        snprintf(out, out_len, "%uK", value / 1024u);
    } else {
        snprintf(out, out_len, "%u", value);
    }
}

static void render(void)
{
    char text[16];

    api->gfx_clear();
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            int x = GRID_X + c * TILE;
            int y = GRID_Y + r * TILE;
            if (g.cell[r][c] == 0u) {
                api->gfx_px(x + TILE / 2 - 1, y + TILE / 2 - 1, true);
                continue;
            }
            api->gfx_frame(x, y, TILE - 1, TILE - 1);
            tile_label(g.cell[r][c], text, sizeof(text));
            int tw = api->gfx_text_w(text);
            api->gfx_text(x + (TILE - 1 - tw) / 2, y + (TILE - 1 - 5) / 2, text);
        }
    }
    int px = GRID_X + N * TILE + 4;
    api->gfx_text(px, 4, "SCORE");
    snprintf(text, sizeof(text), "%d", g.score);
    api->gfx_text(px, 11, text);
    api->gfx_text(px, 24, "BEST");
    snprintf(text, sizeof(text), "%d", api->store_get(GAMES_KV_2048_HS, 0));
    api->gfx_text(px, 31, text);
    api->gfx_text(px, 44, "TILE");
    tile_label((uint8_t)g.max_exp, text, sizeof(text));
    api->gfx_text(px, 51, text);
    api->gfx_flush();
}

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
    return (int)index;
}

static void save_records(void)
{
    if (g.score > api->store_get(GAMES_KV_2048_HS, 0)) {
        api->store_set(GAMES_KV_2048_HS, g.score);
    }
    int best_tile = api->store_get(GAMES_KV_2048_TILE, 0);
    if ((1 << g.max_exp) > best_tile) {
        api->store_set(GAMES_KV_2048_TILE, 1 << g.max_exp);
        if (best_tile > 0) {
            static const jpp_buzzer_note_t k_tile[] = { {880,60},{1100,100} };
            api->sfx_seq(k_tile, 2);
        }
    }
}

static void reset_game(void)
{
    memset(&g, 0, sizeof(g));
    spawn_tile();
    spawn_tile();
}

static bool play_session(void)
{
    reset_game();
    while (!g.game_over) {
        render();

        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        jpp_sdk_wait_key(s_ctx, 250u, &key);

        int dir = -1;
        switch (key) {
        case JPP_SDK_KEY_LEFT:  dir = 0; break;
        case JPP_SDK_KEY_RIGHT: dir = 1; break;
        case JPP_SDK_KEY_UP:    dir = 2; break;
        case JPP_SDK_KEY_DOWN:  dir = 3; break;
        case JPP_SDK_KEY_CENTER_LONG: {
            int action = pause_menu();
            if (action == 1) { reset_game(); }
            if (action == 2) { save_records(); return false; }
            continue;
        }
        default: continue;
        }

        int before_max = g.max_exp;
        if (!slide(dir)) {
            continue;
        }
        api->sfx_tone(200u, 10u);
        spawn_tile();
        save_records();

        if (g.max_exp >= 11 && before_max < 11 && !g.won_shown) {
            g.won_shown = true;
            static const jpp_buzzer_note_t k_win[] =
                { {880,70},{1100,70},{1320,70},{1760,200} };
            api->sfx_seq(k_win, 4);
            jpp_sdk_ui_result_t ui;
            jpp_sdk_dialog(s_ctx, "2048!",
                           "You made the 2048 tile. OK to keep going.", &ui);
            if (ui == JPP_SDK_UI_BACK) { return false; }
        }
        if (!any_moves()) {
            g.game_over = true;
        }
    }

    static const jpp_buzzer_note_t k_over[] =
        { {784,120},{659,120},{523,120},{392,250} };
    api->sfx_seq(k_over, 4);
    save_records();

    char text[48];
    snprintf(text, sizeof(text), "Score %d  Best %d. OK = play again.",
             g.score, api->store_get(GAMES_KV_2048_HS, 0));
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
    while (play_session()) { }
}
