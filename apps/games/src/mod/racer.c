/*
 * Racer — Games-app module.
 *
 * Vertical dodge: your 7×8 car sits at the bottom of a walled road and
 * oncoming cars fall from the top, getting faster and denser as the score
 * climbs. LEFT/RIGHT steer; score = cars dodged. The dashed center line
 * scrolls to sell the speed.
 *
 * Sounds: small milestone flourish every 10 points · three-note crash.
 */

#include "games_api.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const games_api_t *api;
static jpp_sdk_context_t *s_ctx;

#define TICK_MS 33u
#define ROAD_L 24
#define ROAD_R 104
#define CAR_W 7
#define CAR_H 8
#define CAR_Y 54
#define OBST_MAX 6

typedef struct {
    int x, y;                /* top-left; y in Q4 for sub-pixel speed */
    bool active;
    bool counted;
} obstacle_t;

typedef struct {
    int car_x;
    obstacle_t obst[OBST_MAX];
    int score;
    int spawn_cooldown;      /* ticks until the next spawn attempt */
    int scroll;              /* center-line animation phase */
    bool dead;
} racer_t;

static racer_t g;

#define Q 4

static int fall_speed(void)
{
    int v = 32 + g.score;    /* 2 px/tick + 1/16 px per point, capped */
    return v > 80 ? 80 : v;  /* 5 px/tick max */
}

static void reset_game(void)
{
    memset(&g, 0, sizeof(g));
    g.car_x = (ROAD_L + ROAD_R - CAR_W) / 2;
    g.spawn_cooldown = 20;
}

static void spawn_obstacle(void)
{
    for (int i = 0; i < OBST_MAX; i++) {
        if (g.obst[i].active) { continue; }
        g.obst[i].active = true;
        g.obst[i].counted = false;
        g.obst[i].x = ROAD_L + 2 +
                      (int)api->rng((uint32_t)(ROAD_R - ROAD_L - CAR_W - 4));
        g.obst[i].y = -(CAR_H << Q);
        return;
    }
}

static void step(void)
{
    g.scroll = (g.scroll + (fall_speed() >> Q)) % 12;

    if (--g.spawn_cooldown <= 0) {
        spawn_obstacle();
        int interval = 24 - g.score / 8;
        if (interval < 10) { interval = 10; }
        g.spawn_cooldown = interval + (int)api->rng(8u);
    }

    for (int i = 0; i < OBST_MAX; i++) {
        obstacle_t *o = &g.obst[i];
        if (!o->active) { continue; }
        o->y += fall_speed();
        int py = o->y >> Q;
        if (!o->counted && py > CAR_Y + CAR_H) {
            o->counted = true;
            g.score++;
            if (g.score % 10 == 0) {
                static const jpp_buzzer_note_t k_ten[] = { {880,30},{1100,50} };
                api->sfx_seq(k_ten, 2);
            }
        }
        if (py > 64) {
            o->active = false;
            continue;
        }
        /* collision with the player car */
        if (o->x < g.car_x + CAR_W && o->x + CAR_W > g.car_x &&
            py < CAR_Y + CAR_H && py + CAR_H > CAR_Y) {
            g.dead = true;
            return;
        }
    }
}

static void draw_car(int x, int y)
{
    api->gfx_rect(x + 1, y, CAR_W - 2, CAR_H, true);
    api->gfx_rect(x, y + 1, CAR_W, 2, true);       /* front axle */
    api->gfx_rect(x, y + CAR_H - 3, CAR_W, 2, true); /* rear axle */
}

static void render(void)
{
    char text[16];

    api->gfx_clear();
    api->gfx_vline(ROAD_L - 2, 0, 64, true);
    api->gfx_vline(ROAD_R + 1, 0, 64, true);
    for (int y = -g.scroll; y < 64; y += 12) {
        api->gfx_vline(64, y < 0 ? 0 : y, y < 0 ? 6 + y : 6, true);
    }
    draw_car(g.car_x, CAR_Y);
    for (int i = 0; i < OBST_MAX; i++) {
        if (g.obst[i].active && (g.obst[i].y >> Q) > -CAR_H) {
            draw_car(g.obst[i].x, g.obst[i].y >> Q);
        }
    }
    snprintf(text, sizeof(text), "%d", g.score);
    api->gfx_text(2, 1, text);
    api->gfx_flush();
}

static int save_score(void)
{
    int best = api->store_get(GAMES_KV_RACER_HS, 0);
    if (g.score > best) {
        best = g.score;
        api->store_set(GAMES_KV_RACER_HS, best);
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
            case JPP_SDK_KEY_LEFT:
                g.car_x -= 4;
                if (g.car_x < ROAD_L) { g.car_x = ROAD_L; }
                break;
            case JPP_SDK_KEY_RIGHT:
                g.car_x += 4;
                if (g.car_x > ROAD_R - CAR_W) { g.car_x = ROAD_R - CAR_W; }
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

        step();
        render();
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }

    static const jpp_buzzer_note_t k_crash[] = { {400,80},{250,90},{150,250} };
    api->sfx_seq(k_crash, 3);

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
