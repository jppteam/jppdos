/*
 * Snake — Games-app module.
 *
 * Classic rules on a 32×16 grid (4 px cells over the 128×64 canvas): walls
 * and the snake's own body kill, every food speeds the game up by 5 ms
 * (150 ms floor 60 ms). Score = food eaten.
 *
 * Sounds: 988 Hz blip per food (a small two-note flourish every fifth one),
 * descending run on death.
 */

#include "games_api.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const games_api_t *api;
static jpp_sdk_context_t *s_ctx;

#define GW 32
#define GH 16
#define CELLS (GW * GH)

typedef struct {
    uint16_t body[CELLS];   /* ring buffer of cells, cell = x + GW*y */
    int head, tail, len;
    uint8_t occ[CELLS / 8]; /* occupancy bitmask for O(1) collision */
    int dx, dy;
    int pend_dx, pend_dy;   /* applied at the next step (no 180° turns) */
    uint16_t food;
    int score;
    bool dead;
} snake_t;

static snake_t g;

static bool occ_get(uint16_t cell) { return (g.occ[cell / 8] >> (cell % 8)) & 1u; }
static void occ_set(uint16_t cell, bool on)
{
    if (on) { g.occ[cell / 8] |= (uint8_t)(1u << (cell % 8)); }
    else    { g.occ[cell / 8] &= (uint8_t)~(1u << (cell % 8)); }
}

static void place_food(void)
{
    do {
        g.food = (uint16_t)api->rng(CELLS);
    } while (occ_get(g.food));
}

static void reset_game(void)
{
    memset(&g, 0, sizeof(g));
    g.dx = 1;
    g.pend_dx = 1;
    /* start as a 3-cell snake in the middle, moving right */
    for (int i = 0; i < 3; i++) {
        uint16_t cell = (uint16_t)((13 + i) + GW * 8);
        g.body[g.head] = cell;
        occ_set(cell, true);
        if (i < 2) { g.head = (g.head + 1) % CELLS; }
    }
    g.len = 3;
    place_food();
}

static void step(void)
{
    g.dx = g.pend_dx;
    g.dy = g.pend_dy;

    int hx = g.body[g.head] % GW + g.dx;
    int hy = g.body[g.head] / GW + g.dy;
    if (hx < 0 || hx >= GW || hy < 0 || hy >= GH) {
        g.dead = true;
        return;
    }
    uint16_t cell = (uint16_t)(hx + GW * hy);

    bool grow = (cell == g.food);
    if (!grow) {
        /* free the tail first so chasing it is legal */
        occ_set(g.body[g.tail], false);
        g.tail = (g.tail + 1) % CELLS;
    }
    if (occ_get(cell)) {
        g.dead = true;
        return;
    }
    g.head = (g.head + 1) % CELLS;
    g.body[g.head] = cell;
    occ_set(cell, true);

    if (grow) {
        g.len++;
        g.score++;
        if (g.score % 5 == 0) {
            static const jpp_buzzer_note_t k_five[] = { {988,40},{1319,60} };
            api->sfx_seq(k_five, 2);
        } else {
            api->sfx_tone(988u, 30u);
        }
        place_food();
    }
}

static void render(void)
{
    char text[16];

    api->gfx_clear();
    int idx = g.tail;
    for (int i = 0; i < g.len; i++) {
        int x = g.body[idx] % GW;
        int y = g.body[idx] / GW;
        api->gfx_rect(x * 4, y * 4, 3, 3, true);
        idx = (idx + 1) % CELLS;
    }
    api->gfx_frame(g.food % GW * 4, g.food / GW * 4, 4, 4);
    snprintf(text, sizeof(text), "%d", g.score);
    api->gfx_text(1, 1, text);
    api->gfx_flush();
}

static int save_score(void)
{
    int best = api->store_get(GAMES_KV_SNAKE_HS, 0);
    if (g.score > best) {
        best = g.score;
        api->store_set(GAMES_KV_SNAKE_HS, best);
    }
    return best;
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
    if ((int)index == 2) { save_score(); }
    return (int)index;
}

static bool play_session(void)
{
    reset_game();
    while (!g.dead) {
        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        while (jpp_sdk_poll_key(s_ctx, &key) == JPP_SDK_STATUS_OK &&
               key != JPP_SDK_KEY_NONE) {
            switch (key) {
            case JPP_SDK_KEY_UP:    if (g.dy != 1)  { g.pend_dx = 0;  g.pend_dy = -1; } break;
            case JPP_SDK_KEY_DOWN:  if (g.dy != -1) { g.pend_dx = 0;  g.pend_dy = 1;  } break;
            case JPP_SDK_KEY_LEFT:  if (g.dx != 1)  { g.pend_dx = -1; g.pend_dy = 0;  } break;
            case JPP_SDK_KEY_RIGHT: if (g.dx != -1) { g.pend_dx = 1;  g.pend_dy = 0;  } break;
            case JPP_SDK_KEY_CENTER_LONG: {
                int action = pause_menu();
                if (action == 1) { reset_game(); }
                if (action == 2) { return false; }
                break;
            }
            default: break;
            }
            key = JPP_SDK_KEY_NONE;
        }

        step();
        render();
        int interval = 150 - g.score * 5;
        if (interval < 60) { interval = 60; }
        vTaskDelay(pdMS_TO_TICKS((uint32_t)interval));
    }

    static const jpp_buzzer_note_t k_dead[] = { {600,100},{450,100},{300,200} };
    api->sfx_seq(k_dead, 3);

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
    while (play_session()) { }
}
