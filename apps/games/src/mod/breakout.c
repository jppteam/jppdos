/*
 * Breakout — Games-app module.
 *
 * 10×5 brick wall, 16×3 paddle, 2×2 ball in Q4 fixed point, 3 lives.
 * Bricks score by row (top row 5 … bottom row 1, times the level). Clearing
 * the wall advances the level: the wall rebuilds and the serve gets faster.
 * Paddle english: the bounce angle follows where the ball hits the paddle.
 *
 * Sounds: brick pitch rises with the row (660–1320 Hz) · paddle 440 Hz ·
 * wall 330 Hz tap · life lost two-note drop · level-clear fanfare ·
 * descending game-over run.
 */

#include "games_api.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const games_api_t *api;
static jpp_sdk_context_t *s_ctx;

#define TICK_MS 33u
#define Q 4                  /* fixed point shift */
#define BRICK_COLS 10
#define BRICK_ROWS 5
#define BRICK_W 12           /* 11 px brick + 1 px gap */
#define BRICK_H 5            /* 4 px brick + 1 px gap */
#define WALL_X 4
#define WALL_Y 8
#define PADDLE_W 16
#define PADDLE_Y 60
#define BALL_MAX_V (40)      /* 2.5 px/tick in Q4 */

typedef struct {
    uint8_t bricks[BRICK_ROWS][BRICK_COLS];
    int bricks_left;
    int paddle_x;
    int bx, by, bvx, bvy;    /* ball pos/vel, Q4 */
    int score, lives, level;
    bool ball_held;          /* waiting on the paddle for a serve */
    bool game_over;
} breakout_t;

static breakout_t g;

static void build_wall(void)
{
    memset(g.bricks, 1, sizeof(g.bricks));
    g.bricks_left = BRICK_ROWS * BRICK_COLS;
}

static void hold_ball(void)
{
    g.ball_held = true;
    g.bx = (g.paddle_x + PADDLE_W / 2) << Q;
    g.by = (PADDLE_Y - 3) << Q;
}

static void serve(void)
{
    int speed = 13 + g.level * 2;          /* ~0.8 px/tick + level bonus */
    if (speed > 24) { speed = 24; }
    g.bvx = (int)api->rng(2u) ? speed / 2 : -speed / 2;
    g.bvy = -speed;
    g.ball_held = false;
}

static void reset_game(void)
{
    memset(&g, 0, sizeof(g));
    g.lives = 3;
    g.level = 1;
    g.paddle_x = (128 - PADDLE_W) / 2;
    build_wall();
    hold_ball();
}

static void speed_up(void)
{
    /* +5 % per paddle hit, capped */
    g.bvx += g.bvx / 20;
    g.bvy += g.bvy / 20;
    if (g.bvx >  BALL_MAX_V) { g.bvx =  BALL_MAX_V; }
    if (g.bvx < -BALL_MAX_V) { g.bvx = -BALL_MAX_V; }
    if (g.bvy >  BALL_MAX_V) { g.bvy =  BALL_MAX_V; }
    if (g.bvy < -BALL_MAX_V) { g.bvy = -BALL_MAX_V; }
}

static void step_ball(void)
{
    if (g.ball_held) {
        g.bx = (g.paddle_x + PADDLE_W / 2) << Q;
        return;
    }
    g.bx += g.bvx;
    g.by += g.bvy;
    int px = g.bx >> Q;
    int py = g.by >> Q;

    /* walls */
    if (px <= 0)   { g.bx = 0;        g.bvx = -g.bvx; api->sfx_tone(330u, 10u); }
    if (px >= 126) { g.bx = 126 << Q; g.bvx = -g.bvx; api->sfx_tone(330u, 10u); }
    if (py <= 0)   { g.by = 0;        g.bvy = -g.bvy; api->sfx_tone(330u, 10u); }

    /* bricks */
    px = g.bx >> Q;
    py = g.by >> Q;
    if (py >= WALL_Y && py < WALL_Y + BRICK_ROWS * BRICK_H) {
        int row = (py - WALL_Y) / BRICK_H;
        int col = (px + 1 - WALL_X) / BRICK_W;
        if (col >= 0 && col < BRICK_COLS && g.bricks[row][col]) {
            g.bricks[row][col] = 0;
            g.bricks_left--;
            g.score += (BRICK_ROWS - row) * g.level;
            g.bvy = -g.bvy;
            api->sfx_tone(660u + (uint32_t)(BRICK_ROWS - 1 - row) * 165u, 15u);
            if (g.bricks_left == 0) {
                static const jpp_buzzer_note_t k_clear[] =
                    { {660,80},{880,80},{1100,80},{1320,160} };
                api->sfx_seq(k_clear, 4);
                g.level++;
                build_wall();
                hold_ball();
                return;
            }
        }
    }

    /* paddle */
    if (g.bvy > 0 && py >= PADDLE_Y - 2 && py <= PADDLE_Y &&
        px + 2 >= g.paddle_x && px <= g.paddle_x + PADDLE_W) {
        int offset = (px + 1) - (g.paddle_x + PADDLE_W / 2);  /* -8..8 */
        g.bvy = -g.bvy;
        g.bvx += offset * 2;
        speed_up();
        api->sfx_tone(440u, 15u);
    }

    /* missed */
    if (py > 64) {
        g.lives--;
        if (g.lives <= 0) {
            g.game_over = true;
        } else {
            static const jpp_buzzer_note_t k_life[] = { {330,120},{220,200} };
            api->sfx_seq(k_life, 2);
            hold_ball();
        }
    }
}

static void render(void)
{
    char text[16];

    api->gfx_clear();
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (g.bricks[r][c]) {
                api->gfx_rect(WALL_X + c * BRICK_W, WALL_Y + r * BRICK_H,
                              BRICK_W - 1, BRICK_H - 1, true);
            }
        }
    }
    api->gfx_rect(g.paddle_x, PADDLE_Y, PADDLE_W, 3, true);
    if ((g.by >> Q) <= 64) {
        api->gfx_rect(g.bx >> Q, g.by >> Q, 2, 2, true);
    }
    snprintf(text, sizeof(text), "%d", g.score);
    api->gfx_text(1, 1, text);
    for (int i = 0; i < g.lives; i++) {
        api->gfx_rect(120 - i * 4, 1, 2, 2, true);
    }
    if (g.ball_held) {
        api->gfx_text(34, 34, "PRESS OK TO SERVE");
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
    while (!g.game_over) {
        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        while (jpp_sdk_poll_key(s_ctx, &key) == JPP_SDK_STATUS_OK &&
               key != JPP_SDK_KEY_NONE) {
            switch (key) {
            case JPP_SDK_KEY_LEFT:
                g.paddle_x -= 5;
                if (g.paddle_x < 0) { g.paddle_x = 0; }
                break;
            case JPP_SDK_KEY_RIGHT:
                g.paddle_x += 5;
                if (g.paddle_x > 128 - PADDLE_W) { g.paddle_x = 128 - PADDLE_W; }
                break;
            case JPP_SDK_KEY_CENTER:
                if (g.ball_held) { serve(); }
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

        step_ball();
        render();
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }

    static const jpp_buzzer_note_t k_over[] =
        { {784,120},{659,120},{523,120},{392,250} };
    api->sfx_seq(k_over, 4);

    int best = api->store_get(GAMES_KV_BREAKOUT_HS, 0);
    if (g.score > best) {
        best = g.score;
        api->store_set(GAMES_KV_BREAKOUT_HS, best);
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
