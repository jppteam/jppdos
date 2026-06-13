#pragma once

/*
 * games_api — the contract between the Games hub (games.bin) and its
 * dynamically loaded game modules (<name>.mod.bin).
 *
 * Each game is a separate ELF module the hub loads on demand with
 * jpp_sdk_module_load(), so only the hub plus the one running game ever
 * occupy the app pool. Modules resolve firmware symbols (jpp_sdk_*) through
 * the native loader like any app; everything hub-private — the shared shadow
 * framebuffer, the persisted-store helpers, the mute-aware sound effects and
 * the BLE multiplayer session — comes through this function table instead.
 *
 * A module implements:
 *     void jpp_module_entry(jpp_sdk_context_t *ctx, void *api);
 * casts `api` to (const games_api_t *) and must check `version` before use.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpp_sdk_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAMES_API_VERSION 1u

/* ---- Persisted-store keys (hub KV, /sd/apps/games/.kv.json) --------------- */
#define GAMES_KV_TETRIS_HS   "tetris_hs"    /* best Tetris score              */
#define GAMES_KV_PONG_SP_HS  "pong_sp_hs"   /* best wins in one SP session    */
#define GAMES_KV_PONG_MP_HS  "pong_mp_hs"   /* best wins in one MP session    */
#define GAMES_KV_SNAKE_HS    "snake_hs"     /* most food in one run           */
#define GAMES_KV_BREAKOUT_HS "breakout_hs"  /* best Breakout score            */
#define GAMES_KV_2048_HS     "g2048_hs"     /* best 2048 score                */
#define GAMES_KV_2048_TILE   "g2048_tile"   /* best 2048 tile reached         */
#define GAMES_KV_FLAPPY_HS   "flappy_hs"    /* most pipes in one run          */
#define GAMES_KV_RACER_HS    "racer_hs"     /* most cars dodged in one run    */
#define GAMES_KV_C4_WINS     "c4_mp_wins"   /* total Connect-4 MP wins        */
#define GAMES_KV_BSHIP_WINS  "bship_mp_wins" /* total Battleship MP wins      */
#define GAMES_KV_TETRIS_ROT  "tetris_rot"   /* 1 = rotated (default), 0 = normal */
#define GAMES_KV_SOUND       "sound_on"     /* 1 = SFX on (default), 0 = mute */

/* ---- BLE multiplayer ------------------------------------------------------- */

/* Game ids carried in the discovery advertisement so games only pair with
   their own kind. */
#define GAMES_MP_ID_PONG   1u
#define GAMES_MP_ID_C4     2u
#define GAMES_MP_ID_BSHIP  3u

typedef enum {
    GAMES_MP_NONE = 0,   /* discovery cancelled or failed — no session */
    GAMES_MP_HOST,       /* we advertised, peer connected to us (authority) */
    GAMES_MP_CLIENT,     /* we found an advertiser and connected to it */
} games_mp_role_t;

/* ---- The hub function table ------------------------------------------------ */
/*
 * gfx: a 1 KB shadow framebuffer over the fullscreen 128×64 canvas with a
 * 3×5 font (digits, A–Z, basic punctuation; lowercase folds to caps).
 * Coordinates are logical: 128×64 normally, 64×128 after set_rotation(true)
 * (content drawn for a device held sideways). flush() pushes dirty rows to
 * the SDK canvas and re-enters fullscreen mode if a modal UI helper (dialog/
 * list) dropped it, so games may use jpp_sdk_dialog freely between frames.
 *
 * sfx: asynchronous (never blocks the game loop) and silenced when the user
 * turned Sound off in the hub settings — always use these over jpp_sdk_buzzer.
 *
 * mp: BLE session for two-device play. mp_discover() blocks alternating
 * connectable-advertise and scan phases (with UI + BACK to cancel) until a
 * role is settled. Hosts exchange via mp_host_recv/mp_host_send; clients via
 * mp_client_send/mp_client_recv (each client call is one blocking GATT round
 * trip, ~30–75 ms). Packet formats and validation are game-defined.
 * mp_end() tears the session down (safe to call in any state).
 */
typedef struct games_api {
    uint16_t version;            /* GAMES_API_VERSION */

    /* gfx */
    void (*gfx_clear)(void);
    void (*gfx_px)(int x, int y, bool on);
    void (*gfx_rect)(int x, int y, int w, int h, bool on);   /* filled */
    void (*gfx_frame)(int x, int y, int w, int h);           /* 1 px outline */
    void (*gfx_hline)(int x, int y, int w, bool on);
    void (*gfx_vline)(int x, int y, int h, bool on);
    void (*gfx_text)(int x, int y, const char *s);           /* 3×5 font */
    int  (*gfx_text_w)(const char *s);                       /* px width */
    void (*gfx_flush)(void);
    void (*gfx_set_rotation)(bool rotated);
    int  (*gfx_width)(void);
    int  (*gfx_height)(void);

    /* persisted store (int over the hub's KV) */
    int  (*store_get)(const char *key, int fallback);
    void (*store_set)(const char *key, int value);

    /* uniform random in [0, bound); bound 0 returns the raw 32-bit value */
    uint32_t (*rng)(uint32_t bound);

    /* sound effects */
    void (*sfx_tone)(uint32_t freq_hz, uint32_t duration_ms);
    void (*sfx_seq)(const jpp_buzzer_note_t *notes, size_t count);

    /* BLE multiplayer session */
    games_mp_role_t (*mp_discover)(jpp_sdk_context_t *ctx, uint8_t game_id);
    bool (*mp_host_recv)(jpp_sdk_context_t *ctx, uint8_t *buf, size_t buf_len,
                         size_t *out_len, uint32_t timeout_ms);
    bool (*mp_host_send)(jpp_sdk_context_t *ctx, const uint8_t *data, size_t len);
    bool (*mp_client_send)(jpp_sdk_context_t *ctx, const uint8_t *data, size_t len);
    bool (*mp_client_recv)(jpp_sdk_context_t *ctx, uint8_t *buf, size_t buf_len,
                           size_t *out_len);
    void (*mp_end)(jpp_sdk_context_t *ctx);
} games_api_t;

#ifdef __cplusplus
}
#endif
