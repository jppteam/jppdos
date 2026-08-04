/*
 * Pong — Games-app module. Single-player vs the computer and two-device
 * multiplayer over the hub's BLE session.
 *
 * Both modes are endless rounds: every miss scores a round for the other
 * side, the ball re-serves, and the session runs until someone quits.
 * "Score" is the rounds you won in one session; losses are shown but never
 * subtract anything. SP and MP records are stored separately
 * (GAMES_KV_PONG_SP_HS / GAMES_KV_PONG_MP_HS).
 *
 * Multiplayer: discovery settles HOST (advertiser, left paddle, runs the
 * 30 Hz physics and owns the score) and CLIENT (right paddle, sends inputs,
 * renders host snapshots). The host validates every client packet — paddle
 * bounds, monotonic sequence numbers, and a movement-rate limit
 * (|Δy| ≤ 2 px/tick · elapsed + 4). A sliding violation counter (+1 bad,
 * −1 clean) ending a session at 5 is the basic anti-cheat: the host shows
 * "Session ended: abnormal movement", the client is told via the final state
 * packet, and neither side records the session in the high score.
 *
 * Sounds: paddle hit pitch rises with ball speed · 440 Hz wall tap · round
 * won/lost jingles · serve count-in · anticheat alarm.
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
#define PADDLE_H 14
#define PLAY_TOP 7           /* play area starts below the score header row */
#define PLAY_BOTTOM 62       /* ball (2 px) bottom edge reaches row 63 */
#define PADDLE_MAX_Y (64 - PADDLE_H)
#define PADDLE_STEP 4
#define LEFT_X 0
#define RIGHT_X 125
#define BALL_MAX_V 40        /* 2.5 px/tick in Q4 */
#define SERVE_DELAY 30       /* ticks between rounds */

/* ---- wire format ------------------------------------------------------------- */

#define PKT_MAGIC_CLIENT 0xA5u
#define PKT_MAGIC_HOST   0x5Au
#define PKT_TYPE_PADDLE  0x02u
#define PKT_TYPE_BYE     0x03u
#define PKT_TYPE_STATE   0x10u

#define END_NONE      0u
#define END_QUIT      1u
#define END_ANTICHEAT 2u
#define END_TIMEOUT   3u

#define CLIENT_PKT_LEN 8u
#define STATE_PKT_LEN  16u

#define AC_MAX_SPEED   2     /* px per host tick */
#define AC_TOLERANCE   4     /* px slack per packet */
#define AC_THRESHOLD   5     /* violations that end the session */

#define HOST_WATCHDOG_TICKS  60   /* 2 s without a valid client packet */
#define CLIENT_STALE_LIMIT   40   /* ~4 s of unchanged/invalid states */

/* ---- shared game state --------------------------------------------------------- */

typedef struct {
    int ball_x, ball_y;      /* Q4 */
    int vx, vy;              /* Q4 per tick */
    int left_y, right_y;     /* paddle tops, px */
    int left_wins, right_wins;
    int serve_timer;         /* >0 while waiting to serve */
    int serve_dir;           /* +1 toward right, -1 toward left */
} pong_t;

static pong_t g;

static void serve_reset(int dir)
{
    g.ball_x = 63 << Q;
    g.ball_y = (28 + (int)api->rng(8u)) << Q;
    int speed = 13;
    g.vx = dir * speed;
    g.vy = ((int)api->rng(11u) - 5) * 2;
    g.serve_timer = SERVE_DELAY;
    g.serve_dir = dir;
}

static void clamp_speed(void)
{
    if (g.vx >  BALL_MAX_V) { g.vx =  BALL_MAX_V; }
    if (g.vx < -BALL_MAX_V) { g.vx = -BALL_MAX_V; }
    if (g.vy >  BALL_MAX_V) { g.vy =  BALL_MAX_V; }
    if (g.vy < -BALL_MAX_V) { g.vy = -BALL_MAX_V; }
}

static void paddle_bounce(int paddle_y)
{
    int offset = (g.ball_y >> Q) + 1 - (paddle_y + PADDLE_H / 2);
    g.vx = -(g.vx + g.vx / 20);  /* flip + 5 % speed-up */
    g.vy += offset * 2;
    clamp_speed();
    uint32_t speed = (uint32_t)(g.vx < 0 ? -g.vx : g.vx);
    api->sfx_tone(880u + speed * 25u, 20u);
}

static const jpp_buzzer_note_t k_round_won[]  = { {660,80},{990,120} };
static const jpp_buzzer_note_t k_round_lost[] = { {330,150},{220,200} };
static const jpp_buzzer_note_t k_alarm[] =
    { {1500,100},{1000,100},{1500,100},{1000,100} };

/* Steps the ball one tick. Returns 0, or ±1 when a side scored
   (+1 = left scored / right missed, −1 = right scored / left missed). */
static int step_ball(void)
{
    if (g.serve_timer > 0) {
        if (--g.serve_timer == 0) {
            serve_reset(g.serve_dir);
            g.serve_timer = 0;
            api->sfx_tone(880u, 100u);
        } else if (g.serve_timer % 10 == 0) {
            api->sfx_tone(440u, 50u);
        }
        return 0;
    }

    g.ball_x += g.vx;
    g.ball_y += g.vy;

    /* Only reflect when moving toward the wall, otherwise a slow ball pinned at
       the boundary re-triggers the bounce every tick and sticks (the reset
       leaves py exactly on the threshold). */
    int py = g.ball_y >> Q;
    if (g.vy < 0 && py <= PLAY_TOP)    { g.ball_y = PLAY_TOP << Q;    g.vy = -g.vy; api->sfx_tone(440u, 15u); }
    if (g.vy > 0 && py >= PLAY_BOTTOM) { g.ball_y = PLAY_BOTTOM << Q; g.vy = -g.vy; api->sfx_tone(440u, 15u); }

    int px = g.ball_x >> Q;
    if (g.vx < 0 && px <= LEFT_X + 3 && px >= LEFT_X &&
        py + 2 >= g.left_y && py <= g.left_y + PADDLE_H) {
        g.ball_x = (LEFT_X + 3) << Q;
        paddle_bounce(g.left_y);
    }
    if (g.vx > 0 && px + 2 >= RIGHT_X && px <= RIGHT_X + 2 &&
        py + 2 >= g.right_y && py <= g.right_y + PADDLE_H) {
        g.ball_x = (RIGHT_X - 2) << Q;
        paddle_bounce(g.right_y);
    }

    if (px < -2) {  /* left missed — right scored */
        g.right_wins++;
        serve_reset(-1);
        return -1;
    }
    if (px > 128) { /* right missed — left scored */
        g.left_wins++;
        serve_reset(1);
        return 1;
    }
    return 0;
}

static void render(bool we_are_left, const char *peer_label)
{
    char text[24];

    api->gfx_clear();
    api->gfx_rect(LEFT_X, g.left_y, 3, PADDLE_H, true);
    api->gfx_rect(RIGHT_X, g.right_y, 3, PADDLE_H, true);
    for (int y = PLAY_TOP; y < 64; y += 6) {
        api->gfx_vline(63, y, 3, true);
    }
    if (g.serve_timer == 0) {
        api->gfx_rect(g.ball_x >> Q, g.ball_y >> Q, 2, 2, true);
    }
    int we   = we_are_left ? g.left_wins : g.right_wins;
    int they = we_are_left ? g.right_wins : g.left_wins;
    snprintf(text, sizeof(text), "YOU %d - %d %s", we, they, peer_label);
    api->gfx_text(64 - api->gfx_text_w(text) / 2, 1, text);
    /* Separator under the score header so the top bounce reads clearly. */
    api->gfx_hline(0, PLAY_TOP - 1, 128, true);
    api->gfx_flush();
}

static void move_paddle(int *paddle_y, int delta)
{
    *paddle_y += delta;
    if (*paddle_y < PLAY_TOP) { *paddle_y = PLAY_TOP; }
    if (*paddle_y > PADDLE_MAX_Y) { *paddle_y = PADDLE_MAX_Y; }
}

static void save_record(const char *key, int wins)
{
    if (wins > api->store_get(key, 0)) {
        api->store_set(key, wins);
    }
}

/* ---- single player --------------------------------------------------------------- */

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

static void sp_reset(void)
{
    memset(&g, 0, sizeof(g));
    g.left_y = g.right_y = (64 - PADDLE_H) / 2;
    serve_reset((int)api->rng(2u) ? 1 : -1);
}

static void cpu_step(void)
{
    /* Tracks the ball only while it approaches, capped speed, ±2 px deadzone —
       beatable once the ball is near full speed. */
    if (g.vx <= 0 || g.serve_timer > 0) {
        return;
    }
    int target = (g.ball_y >> Q) - PADDLE_H / 2 + 1;
    int diff = target - g.right_y;
    if (diff > 2)       { move_paddle(&g.right_y, diff > 14 ? 2 : 1); }
    else if (diff < -2) { move_paddle(&g.right_y, diff < -14 ? -2 : -1); }
}

static void play_single(void)
{
    sp_reset();
    for (;;) {
        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        while (jpp_sdk_poll_key(s_ctx, &key) == JPP_SDK_STATUS_OK &&
               key != JPP_SDK_KEY_NONE) {
            switch (key) {
            case JPP_SDK_KEY_UP:   move_paddle(&g.left_y, -PADDLE_STEP); break;
            case JPP_SDK_KEY_DOWN: move_paddle(&g.left_y, PADDLE_STEP);  break;
            case JPP_SDK_KEY_OK_LONG: {
                int action = pause_menu();
                if (action == 1) { sp_reset(); }
                if (action == 2) {
                    save_record(GAMES_KV_PONG_SP_HS, g.left_wins);
                    return;
                }
                break;
            }
            default: break;
            }
            key = JPP_SDK_KEY_NONE;
        }

        cpu_step();
        int scored = step_ball();
        if (scored > 0) {
            api->sfx_seq(k_round_won, 2);
            save_record(GAMES_KV_PONG_SP_HS, g.left_wins);
        } else if (scored < 0) {
            api->sfx_seq(k_round_lost, 2);
        }
        render(true, "CPU");
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

/* ---- multiplayer: host -------------------------------------------------------------- */

typedef struct {
    uint16_t last_seq;
    bool     have_seq;
    uint32_t last_accept_tick;
    int      violations;
} anticheat_t;

/* Validates one client packet. Returns true when the paddle value should be
   applied; bumps the violation counter on abnormal movement. Stale sequence
   numbers are dropped silently (BLE retries are not cheating). */
static bool anticheat_check(anticheat_t *ac, uint16_t seq, int paddle_y,
                            uint32_t now_tick)
{
    if (ac->have_seq && (uint16_t)(seq - ac->last_seq) == 0u) {
        return false;  /* duplicate */
    }
    if (ac->have_seq && (uint16_t)(seq - ac->last_seq) > 0x8000u) {
        return false;  /* stale/reordered */
    }

    bool violation = false;
    if (paddle_y < 0 || paddle_y > PADDLE_MAX_Y) {
        violation = true;
    } else if (ac->have_seq) {
        uint32_t dt = now_tick - ac->last_accept_tick;
        if (dt > 1000u) { dt = 1000u; }
        int max_delta = (int)dt * AC_MAX_SPEED + AC_TOLERANCE;
        int delta = paddle_y - g.right_y;
        if (delta < 0) { delta = -delta; }
        if (delta > max_delta) {
            violation = true;
        }
    }

    if (violation) {
        ac->violations++;
        return false;
    }
    if (ac->violations > 0) {
        ac->violations--;
    }
    ac->last_seq = seq;
    ac->have_seq = true;
    ac->last_accept_tick = now_tick;
    return true;
}

static void host_send_state(uint16_t tick, uint8_t round_event,
                            uint8_t end_reason, uint16_t ack_seq)
{
    uint8_t pkt[STATE_PKT_LEN];
    pkt[0]  = PKT_MAGIC_HOST;
    pkt[1]  = PKT_TYPE_STATE;
    pkt[2]  = (uint8_t)(tick & 0xFFu);
    pkt[3]  = (uint8_t)(tick >> 8);
    pkt[4]  = (uint8_t)(g.serve_timer > 0 ? 0xFFu : (g.ball_x >> Q));
    pkt[5]  = (uint8_t)(g.ball_y >> Q);
    pkt[6]  = (uint8_t)g.left_y;
    pkt[7]  = (uint8_t)g.right_y;
    pkt[8]  = (uint8_t)g.left_wins;
    pkt[9]  = (uint8_t)g.right_wins;
    pkt[10] = round_event;
    pkt[11] = end_reason;
    pkt[12] = (uint8_t)(ack_seq & 0xFFu);
    pkt[13] = (uint8_t)(ack_seq >> 8);
    pkt[14] = 0u;
    pkt[15] = 0u;
    api->mp_host_send(s_ctx, pkt, sizeof(pkt));
}

/* returns the end reason */
static uint8_t play_host(void)
{
    anticheat_t ac = { 0 };
    uint16_t tick = 0u;
    uint32_t idle_ticks = 0u;
    uint8_t  rx[CLIENT_PKT_LEN];

    memset(&g, 0, sizeof(g));
    g.left_y = g.right_y = (64 - PADDLE_H) / 2;
    serve_reset((int)api->rng(2u) ? 1 : -1);

    for (;;) {
        tick++;

        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        while (jpp_sdk_poll_key(s_ctx, &key) == JPP_SDK_STATUS_OK &&
               key != JPP_SDK_KEY_NONE) {
            switch (key) {
            case JPP_SDK_KEY_UP:   move_paddle(&g.left_y, -PADDLE_STEP); break;
            case JPP_SDK_KEY_DOWN: move_paddle(&g.left_y, PADDLE_STEP);  break;
            case JPP_SDK_KEY_OK_LONG:
                host_send_state(tick, 0u, END_QUIT, ac.last_seq);
                vTaskDelay(pdMS_TO_TICKS(300u));
                return END_QUIT;
            default: break;
            }
            key = JPP_SDK_KEY_NONE;
        }

        /* Drain the freshest client packet (clients send at ~10 Hz). */
        size_t rx_len = 0u;
        if (api->mp_host_recv(s_ctx, rx, sizeof(rx), &rx_len, 5u) &&
            rx_len >= CLIENT_PKT_LEN && rx[0] == PKT_MAGIC_CLIENT) {
            if (rx[1] == PKT_TYPE_BYE) {
                return END_QUIT;
            }
            if (rx[1] == PKT_TYPE_PADDLE) {
                idle_ticks = 0u;
                uint16_t seq = (uint16_t)(rx[2] | (rx[3] << 8));
                if (anticheat_check(&ac, seq, (int)rx[4], tick)) {
                    g.right_y = (int)rx[4];
                }
                if (ac.violations >= AC_THRESHOLD) {
                    host_send_state(tick, 0u, END_ANTICHEAT, ac.last_seq);
                    vTaskDelay(pdMS_TO_TICKS(300u));
                    return END_ANTICHEAT;
                }
            }
        } else {
            idle_ticks++;
            if (idle_ticks > HOST_WATCHDOG_TICKS) {
                return END_TIMEOUT;
            }
        }

        int scored = step_ball();
        uint8_t round_event = 0u;
        if (scored > 0) {
            round_event = 1u;
            api->sfx_seq(k_round_won, 2);
            save_record(GAMES_KV_PONG_MP_HS, g.left_wins);
        } else if (scored < 0) {
            round_event = 2u;
            api->sfx_seq(k_round_lost, 2);
        }
        host_send_state(tick, round_event, END_NONE, ac.last_seq);

        render(true, "PEER");
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

/* ---- multiplayer: client -------------------------------------------------------------- */

static bool client_send_paddle(uint16_t seq, int paddle_y, uint8_t type)
{
    uint8_t pkt[CLIENT_PKT_LEN];
    pkt[0] = PKT_MAGIC_CLIENT;
    pkt[1] = type;
    pkt[2] = (uint8_t)(seq & 0xFFu);
    pkt[3] = (uint8_t)(seq >> 8);
    pkt[4] = (uint8_t)paddle_y;
    pkt[5] = 0u;
    pkt[6] = 0u;
    pkt[7] = 0u;
    return api->mp_client_send(s_ctx, pkt, sizeof(pkt));
}

/* returns the end reason */
static uint8_t play_client(void)
{
    uint16_t seq = 0u;
    uint16_t last_state_tick = 0u;
    int      stale = 0;
    int      transport_fail = 0;
    int      my_paddle = (64 - PADDLE_H) / 2;
    uint8_t  rx[64];

    memset(&g, 0, sizeof(g));
    g.left_y = g.right_y = (64 - PADDLE_H) / 2;
    g.serve_timer = 1;  /* hide the ball until the first state arrives */

    for (;;) {
        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        while (jpp_sdk_poll_key(s_ctx, &key) == JPP_SDK_STATUS_OK &&
               key != JPP_SDK_KEY_NONE) {
            switch (key) {
            case JPP_SDK_KEY_UP:   move_paddle(&my_paddle, -PADDLE_STEP); break;
            case JPP_SDK_KEY_DOWN: move_paddle(&my_paddle, PADDLE_STEP);  break;
            case JPP_SDK_KEY_OK_LONG:
                client_send_paddle(++seq, my_paddle, PKT_TYPE_BYE);
                return END_QUIT;
            default: break;
            }
            key = JPP_SDK_KEY_NONE;
        }

        /* One input write + one state read = one network frame (~60–150 ms). */
        bool sent = client_send_paddle(++seq, my_paddle, PKT_TYPE_PADDLE);
        size_t rx_len = 0u;
        bool got = api->mp_client_recv(s_ctx, rx, sizeof(rx), &rx_len);
        if (!sent || !got) {
            if (++transport_fail >= 2) {
                return END_TIMEOUT;
            }
        } else {
            transport_fail = 0;
        }

        if (got && rx_len >= STATE_PKT_LEN &&
            rx[0] == PKT_MAGIC_HOST && rx[1] == PKT_TYPE_STATE) {
            uint16_t state_tick = (uint16_t)(rx[2] | (rx[3] << 8));
            if (state_tick != last_state_tick) {
                last_state_tick = state_tick;
                stale = 0;
            } else {
                stale++;
            }
            g.serve_timer = (rx[4] == 0xFFu) ? 1 : 0;
            g.ball_x  = rx[4] == 0xFFu ? g.ball_x : (int)rx[4] << Q;
            g.ball_y  = (int)rx[5] << Q;
            g.left_y  = (int)rx[6];
            /* Reconcile our paddle to what the host accepted; keep the local
               prediction when the host is just behind our latest input. */
            uint16_t ack = (uint16_t)(rx[12] | (rx[13] << 8));
            if (ack == seq) {
                my_paddle = (int)rx[7];
            }
            g.right_y = my_paddle;
            int old_left = g.left_wins;
            int old_right = g.right_wins;
            g.left_wins  = (int)rx[8];
            g.right_wins = (int)rx[9];
            uint8_t end_reason = rx[11];
            if (end_reason != END_NONE) {
                return end_reason;
            }
            if (g.right_wins > old_right) {
                api->sfx_seq(k_round_won, 2);
                save_record(GAMES_KV_PONG_MP_HS, g.right_wins);
            } else if (g.left_wins > old_left) {
                api->sfx_seq(k_round_lost, 2);
            }
        } else {
            stale++;
        }
        if (stale > CLIENT_STALE_LIMIT) {
            return END_TIMEOUT;
        }

        render(false, "PEER");
        vTaskDelay(pdMS_TO_TICKS(10u));
    }
}

/* ---- multiplayer wrapper --------------------------------------------------------------- */

static void play_multi(void)
{
    games_mp_role_t role = api->mp_discover(s_ctx, GAMES_MP_ID_PONG);
    if (role == GAMES_MP_NONE) {
        return;
    }

    uint8_t end_reason;
    int my_wins;
    if (role == GAMES_MP_HOST) {
        end_reason = play_host();
        my_wins = g.left_wins;
    } else {
        end_reason = play_client();
        my_wins = g.right_wins;
    }
    api->mp_end(s_ctx);

    jpp_sdk_ui_result_t ui;
    char text[64];
    if (end_reason == END_ANTICHEAT) {
        api->sfx_seq(k_alarm, 4);
        /* An abnormal session never counts toward the record. */
        jpp_sdk_dialog(s_ctx,
                       role == GAMES_MP_HOST ? "Session ended" : "Session ended",
                       role == GAMES_MP_HOST
                           ? "Abnormal movement detected. Session ended."
                           : "The host ended the session.", &ui);
        return;
    }
    save_record(GAMES_KV_PONG_MP_HS, my_wins);
    snprintf(text, sizeof(text), "%s  You won %d rounds. Best %d.",
             end_reason == END_TIMEOUT ? "Connection lost." : "Match over.",
             my_wins, api->store_get(GAMES_KV_PONG_MP_HS, 0));
    jpp_sdk_dialog(s_ctx, "Pong", text, &ui);
}

/* ---- entry --------------------------------------------------------------------------- */

void jpp_module_entry(jpp_sdk_context_t *ctx, void *api_ptr)
{
    api = (const games_api_t *)api_ptr;
    if (api == NULL || api->version != GAMES_API_VERSION) {
        return;
    }
    s_ctx = ctx;

    for (;;) {
        static const char *items[] = { "Single player", "Multiplayer" };
        size_t index = 0u, count = 0u;
        jpp_sdk_ui_result_t ui;
        if (jpp_sdk_list(s_ctx, "Pong", items, 2u, false,
                         &index, 1u, &count, &ui) != JPP_SDK_STATUS_OK ||
            ui == JPP_SDK_UI_BACK || count == 0u) {
            return;
        }
        if (index == 0u) {
            play_single();
        } else {
            play_multi();
        }
    }
}
