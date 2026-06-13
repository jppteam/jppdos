/*
 * Games hub — the resident part of the Games app.
 *
 * Presents the game menu, the combined high-scores screen and the settings
 * screen, and chain-loads the selected game module (<name>.mod.bin from the
 * app's own scoped storage) into the app-pool tail via jpp_sdk_module_load.
 * Only the hub plus one game are ever in memory at a time.
 */

#include "games.h"
#include "games_api.h"
#include "games_ble.h"
#include "games_gfx.h"
#include "games_rng.h"
#include "games_sfx.h"
#include "games_store.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- The API table handed to every game module ----------------------------- */

static const games_api_t s_api = {
    .version          = GAMES_API_VERSION,
    .gfx_clear        = games_gfx_clear,
    .gfx_px           = games_gfx_px,
    .gfx_rect         = games_gfx_rect,
    .gfx_frame        = games_gfx_frame,
    .gfx_hline        = games_gfx_hline,
    .gfx_vline        = games_gfx_vline,
    .gfx_text         = games_gfx_text,
    .gfx_text_w       = games_gfx_text_w,
    .gfx_flush        = games_gfx_flush,
    .gfx_set_rotation = games_gfx_set_rotation,
    .gfx_width        = games_gfx_width,
    .gfx_height       = games_gfx_height,
    .store_get        = games_store_get,
    .store_set        = games_store_set,
    .rng              = games_rng,
    .sfx_tone         = games_sfx_tone,
    .sfx_seq          = games_sfx_seq,
    .mp_discover      = games_ble_discover,
    .mp_host_recv     = games_ble_host_recv,
    .mp_host_send     = games_ble_host_send,
    .mp_client_send   = games_ble_client_send,
    .mp_client_recv   = games_ble_client_recv,
    .mp_end           = games_ble_end,
};

/* ---- Module launcher --------------------------------------------------------- */

static void run_game(jpp_sdk_context_t *ctx, const char *module_file)
{
    void *module = NULL;
    jpp_sdk_ui_result_t ui;

    if (jpp_sdk_module_load(ctx, module_file, &module) != JPP_SDK_STATUS_OK) {
        jpp_sdk_dialog(ctx, "Games",
                       "Could not load the game. Reinstall the app files on "
                       "the SD card.", &ui);
        return;
    }
    jpp_sdk_module_run(ctx, module, (void *)&s_api);
    jpp_sdk_module_unload(ctx, module);
    /* A module may exit mid-session; make sure nothing lingers. */
    games_ble_end(ctx);
    jpp_sdk_buzzer_stop(ctx);
    games_gfx_set_rotation(false);
}

/* ---- High scores screen ------------------------------------------------------- */

typedef struct {
    const char *label;
    const char *key;
} score_row_t;

static const score_row_t k_scores[] = {
    { "TETRIS",     GAMES_KV_TETRIS_HS },
    { "PONG SOLO",  GAMES_KV_PONG_SP_HS },
    { "PONG VS",    GAMES_KV_PONG_MP_HS },
    { "SNAKE",      GAMES_KV_SNAKE_HS },
    { "BREAKOUT",   GAMES_KV_BREAKOUT_HS },
    { "2048",       GAMES_KV_2048_HS },
    { "2048 TILE",  GAMES_KV_2048_TILE },
    { "FLAPPY",     GAMES_KV_FLAPPY_HS },
    { "RACER",      GAMES_KV_RACER_HS },
    { "C4 WINS",    GAMES_KV_C4_WINS },
    { "BSHIP WINS", GAMES_KV_BSHIP_WINS },
};
#define SCORE_ROWS (sizeof(k_scores) / sizeof(k_scores[0]))

/* Read-only scrollable breakdown rendered with the SDK list (system font);
   any key (CENTER / BACK) returns. The label is left-padded so the value
   lines up to the right of the name. */
static void scores_screen(jpp_sdk_context_t *ctx)
{
    static char buf[SCORE_ROWS][24];
    const char *items[SCORE_ROWS];
    for (size_t i = 0u; i < SCORE_ROWS; i++) {
        snprintf(buf[i], sizeof(buf[i]), "%-11s%5d",
                 k_scores[i].label, games_store_get(k_scores[i].key, 0));
        items[i] = buf[i];
    }
    size_t index = 0u, count = 0u;
    jpp_sdk_ui_result_t ui;
    jpp_sdk_list(ctx, "High scores", items, SCORE_ROWS, false,
                 &index, 1u, &count, &ui);
}

/* ---- Settings screen ------------------------------------------------------------ */

static void settings_screen(jpp_sdk_context_t *ctx)
{
    for (;;) {
        bool rotated  = games_store_get(GAMES_KV_TETRIS_ROT, 1) != 0;
        bool sound_on = games_store_get(GAMES_KV_SOUND, 1) != 0;

        char tetris_row[24];
        char sound_row[24];
        snprintf(tetris_row, sizeof(tetris_row), "Tetris: %s",
                 rotated ? "Rotated" : "Normal");
        snprintf(sound_row, sizeof(sound_row), "Sound: %s",
                 sound_on ? "On" : "Off");
        const char *items[] = { tetris_row, sound_row, "Back" };

        size_t index = 0u;
        size_t count = 0u;
        jpp_sdk_ui_result_t ui;
        if (jpp_sdk_list(ctx, "Settings", items, 3u, false,
                         &index, 1u, &count, &ui) != JPP_SDK_STATUS_OK ||
            ui == JPP_SDK_UI_BACK || count == 0u) {
            return;
        }
        if (index == 0u) {
            games_store_set(GAMES_KV_TETRIS_ROT, rotated ? 0 : 1);
        } else if (index == 1u) {
            games_store_set(GAMES_KV_SOUND, sound_on ? 0 : 1);
            games_sfx_set_enabled(!sound_on);
            games_sfx_tone(880u, 40u);  /* audible confirmation when enabling */
        } else {
            return;
        }
    }
}

/* ---- Main menu --------------------------------------------------------------- */

typedef struct {
    const char *label;
    const char *module_file;   /* NULL for hub-internal screens */
    const char *score_key;     /* primary high score shown in the menu, or NULL */
} menu_row_t;

static const menu_row_t k_menu[] = {
    { "Tetris",      "tetris.mod.bin",     GAMES_KV_TETRIS_HS },
    { "Pong",        "pong.mod.bin",       GAMES_KV_PONG_SP_HS },
    { "Snake",       "snake.mod.bin",      GAMES_KV_SNAKE_HS },
    { "Breakout",    "breakout.mod.bin",   GAMES_KV_BREAKOUT_HS },
    { "2048",        "g2048.mod.bin",      GAMES_KV_2048_HS },
    { "Flappy",      "flappy.mod.bin",     GAMES_KV_FLAPPY_HS },
    { "Racer",       "racer.mod.bin",      GAMES_KV_RACER_HS },
    { "Connect-4",   "connect4.mod.bin",   GAMES_KV_C4_WINS },
    { "Battleship",  "battleship.mod.bin", GAMES_KV_BSHIP_WINS },
    { "High scores", NULL,                 NULL },
    { "Settings",    NULL,                 NULL },
};
#define MENU_ROWS (sizeof(k_menu) / sizeof(k_menu[0]))

/* Main menu via the SDK list (system font). Each game's primary high score is
   embedded in the item string, left-padded so it sits to the right of the
   name. Returns the chosen index, or -1 to exit the app. */
static int main_menu(jpp_sdk_context_t *ctx)
{
    static char buf[MENU_ROWS][24];
    const char *items[MENU_ROWS];
    for (size_t i = 0u; i < MENU_ROWS; i++) {
        if (k_menu[i].score_key != NULL) {
            int score = games_store_get(k_menu[i].score_key, 0);
            int label_len = (int)strlen(k_menu[i].label);
            int score_digits = (score == 0) ? 1 : (int)floor(log10(score)) + 1;
            int spaces = 20 - label_len - score_digits;
            if (spaces < 1) spaces = 1; /* always at least one space */
            snprintf(buf[i], sizeof(buf[i]), "%s%*s%d",
                    k_menu[i].label, spaces, "", score);
        } else {
            snprintf(buf[i], sizeof(buf[i]), "%s", k_menu[i].label);
        }
        items[i] = buf[i];
    }

    size_t index = 0u, count = 0u;
    jpp_sdk_ui_result_t ui;
    if (jpp_sdk_list(ctx, "Games", items, MENU_ROWS, false,
                     &index, 1u, &count, &ui) != JPP_SDK_STATUS_OK ||
        ui == JPP_SDK_UI_BACK || count == 0u) {
        return -1;
    }
    return (int)index;
}

void games_run(jpp_sdk_context_t *ctx)
{
    games_gfx_init(ctx);
    games_store_init(ctx);
    games_sfx_init(ctx, games_store_get(GAMES_KV_SOUND, 1) != 0);
    games_rng_seed((uint32_t)xTaskGetTickCount() * 2654435761u + 1u);

    jpp_sdk_wakelock_acquire(ctx);

    for (;;) {
        int index = main_menu(ctx);
        if (index < 0) {
            break;
        }
        if (k_menu[index].module_file != NULL) {
            run_game(ctx, k_menu[index].module_file);
        } else if (index == (int)MENU_ROWS - 2) {
            scores_screen(ctx);
        } else {
            settings_screen(ctx);
        }
    }

    jpp_sdk_wakelock_release(ctx);
    jpp_sdk_request_close(ctx);
}
