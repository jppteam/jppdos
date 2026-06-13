/*
 * Flappy — Games-app module.
 *
 * One-button: UP or OK flaps. The 4×4 bird falls under Q4 gravity; 10 px
 * pipes scroll left at 1 px/tick with a gap that narrows from 24 px to
 * 18 px as the score grows. Score = pipes passed.
 *
 * Sounds: 600 Hz flap tick · 988 Hz point blip · two-note drop on death.
 */

#include "games_api.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const games_api_t *api;
static jpp_sdk_context_t *s_ctx;

#define TICK_MS 33u
#define Q 4
#define BIRD_X 24
#define BIRD_SIZE 4
#define PIPE_W 10
#define PIPE_SPACING 48
#define PIPE_COUNT 3
#define GRAVITY 3            /* Q4 per tick */
#define FLAP_V (-22)         /* Q4 */
#define MAX_FALL 24          /* Q4 */

typedef struct {
    int x;                   /* left edge, px; may go negative while leaving */
    int gap_y;               /* top of the gap */
    bool counted;
} pipe_t;

typedef struct {
    int by, bvy;             /* bird y Q4, velocity Q4 */
    pipe_t pipes[PIPE_COUNT];
    int score;
    bool started;
    bool dead;
} flappy_t;

static flappy_t g;

static int gap_height(void)
{
    int gap = 24 - g.score / 5;
    return gap < 18 ? 18 : gap;
}

static void respawn_pipe(pipe_t *p, int x)
{
    p->x = x;
    p->gap_y = 6 + (int)api->rng((uint32_t)(64 - 12 - gap_height()));
    p->counted = false;
}

static void reset_game(void)
{
    memset(&g, 0, sizeof(g));
    g.by = 28 << Q;
    for (int i = 0; i < PIPE_COUNT; i++) {
        respawn_pipe(&g.pipes[i], 128 + 16 + i * PIPE_SPACING);
    }
}

static void step(void)
{
    g.bvy += GRAVITY;
    if (g.bvy > MAX_FALL) { g.bvy = MAX_FALL; }
    g.by += g.bvy;

    int bird_top = g.by >> Q;
    if (bird_top <= 0 || bird_top + BIRD_SIZE >= 64) {
        g.dead = true;
        return;
    }

    for (int i = 0; i < PIPE_COUNT; i++) {
        pipe_t *p = &g.pipes[i];
        p->x -= 1;
        if (p->x + PIPE_W < 0) {
            respawn_pipe(p, p->x + PIPE_COUNT * PIPE_SPACING);
        }
        if (!p->counted && p->x + PIPE_W < BIRD_X) {
            p->counted = true;
            g.score++;
            api->sfx_tone(988u, 30u);
        }
        /* collision */
        if (p->x < BIRD_X + BIRD_SIZE && p->x + PIPE_W > BIRD_X) {
            if (bird_top < p->gap_y ||
                bird_top + BIRD_SIZE > p->gap_y + gap_height()) {
                g.dead = true;
                return;
            }
        }
    }
}

static void render(void)
{
    char text[16];

    api->gfx_clear();
    for (int i = 0; i < PIPE_COUNT; i++) {
        const pipe_t *p = &g.pipes[i];
        if (p->x >= 128 || p->x + PIPE_W <= 0) { continue; }
        api->gfx_rect(p->x, 0, PIPE_W, p->gap_y, true);
        api->gfx_rect(p->x, p->gap_y + gap_height(), PIPE_W,
                      64 - p->gap_y - gap_height(), true);
    }
    api->gfx_rect(BIRD_X, g.by >> Q, BIRD_SIZE, BIRD_SIZE, true);
    snprintf(text, sizeof(text), "%d", g.score);
    api->gfx_text(62, 1, text);
    if (!g.started) {
        api->gfx_text(34, 28, "PRESS UP TO FLAP");
    }
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

static bool play_session(void)
{
    reset_game();
    while (!g.dead) {
        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        while (jpp_sdk_poll_key(s_ctx, &key) == JPP_SDK_STATUS_OK &&
               key != JPP_SDK_KEY_NONE) {
            switch (key) {
            case JPP_SDK_KEY_UP:
            case JPP_SDK_KEY_CENTER:
                g.started = true;
                g.bvy = FLAP_V;
                api->sfx_tone(600u, 12u);
                break;
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

        if (g.started) {
            step();
        }
        render();
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }

    static const jpp_buzzer_note_t k_dead[] = { {500,80},{300,200} };
    api->sfx_seq(k_dead, 2);

    int best = api->store_get(GAMES_KV_FLAPPY_HS, 0);
    if (g.score > best) {
        best = g.score;
        api->store_set(GAMES_KV_FLAPPY_HS, best);
    }
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
