/*
 * Battleship — Games-app module. BLE two-device only.
 *
 * Two 8×8 grids drawn side by side (7 px cells: left = own fleet, right =
 * targeting). Placement phase: ships of length {4,3,3,2}; the cursor moves
 * with the d-pad, RIGHT-long... actually OK places, and the UP/DOWN-held
 * "rotate" is on the OK key isn't possible, so rotation is on a dedicated
 * key (LEFT_LONG not available) — we rotate with a quick double: pressing
 * OK on an already-selected origin cycles orientation; see place_phase().
 *
 * Hidden-state cheating is the real risk here: a device could lie about a hit
 * to never lose. We defend with a commit–reveal: at battle start each side
 * sends SHA-512(board ‖ nonce); at game end both reveal board+nonce and each
 * verifies the other's commitment. A mismatch voids the result (no win
 * recorded) and is reported. The host additionally validates shot bounds,
 * turn order and duplicate shots (3 strikes ends the session).
 *
 * Wire (raw bytes, hub-ferried; client hex-encoded by the SDK):
 *   COMMIT : ['M'][hash 64]
 *   SHOT   : ['S'][seq][r][c]
 *   RESULT : ['R'][seq][r][c][outcome]      outcome 0 miss /1 hit /2 sunk-all(win)
 *   REVEAL : ['V'][nonce 16][board 64]
 *   BYE    : ['X']
 * Both peers are symmetric here (each keeps its own board and answers shots),
 * but the HOST referees turn order and strike counting.
 */

#include "games_api.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sodium/crypto_hash_sha512.h"
#include "sodium/randombytes.h"

static const games_api_t *api;
static jpp_sdk_context_t *s_ctx;

#define N 8
#define CELL 7
#define OWN_X 2
#define TGT_X 70
#define GRID_Y 6

#define MSG_COMMIT 'M'
#define MSG_SHOT   'S'
#define MSG_RESULT 'R'
#define MSG_REVEAL 'V'
#define MSG_BYE    'X'

#define OUT_MISS 0u
#define OUT_HIT  1u
#define OUT_WIN  2u

#define END_NONE      0u
#define END_QUIT      1u
#define END_ANTICHEAT 2u
#define END_TIMEOUT   3u
#define END_CHEAT     4u   /* commitment mismatch on reveal */

#define AC_THRESHOLD  3

/* cell states */
#define OWN_EMPTY 0u
#define OWN_SHIP  1u
#define OWN_HIT   2u   /* our ship that was hit */
#define OWN_MISS  3u   /* enemy shot that missed us */

#define TGT_UNKNOWN 0u
#define TGT_MISS    1u
#define TGT_HIT     2u

static const uint8_t k_ships[] = { 4, 3, 3, 2 };
#define NUM_SHIPS (sizeof(k_ships) / sizeof(k_ships[0]))
#define TOTAL_SHIP_CELLS (4 + 3 + 3 + 2)

typedef struct {
    uint8_t own[N][N];       /* OWN_* */
    uint8_t tgt[N][N];       /* TGT_* */
    uint8_t nonce[16];
    uint8_t commit[64];      /* our SHA-512(board ‖ nonce) */
    uint8_t peer_commit[64];
    bool    peer_committed;
    int     own_hits_taken;  /* enemy hits on us */
    int     enemy_hits;      /* our hits on enemy */
    int     cur_r, cur_c;    /* targeting cursor */
} bship_t;

static bship_t b;

static const jpp_buzzer_note_t k_hit[]  = { {880,50},{1100,80} };
static const jpp_buzzer_note_t k_sunk[] = { {660,70},{880,70},{1100,70},{1320,140} };
static const jpp_buzzer_note_t k_win[]  = { {660,80},{880,80},{1100,80},{1320,200} };
static const jpp_buzzer_note_t k_lose[] = { {440,120},{330,120},{220,250} };

/* ---- ship board packing for the commitment ----------------------------------- */
/* The committed board is the ship layout only (1 = ship), 64 bytes. */

static void pack_ship_board(uint8_t out[N * N])
{
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            out[r * N + c] = (b.own[r][c] == OWN_SHIP || b.own[r][c] == OWN_HIT)
                                 ? 1u : 0u;
        }
    }
}

static void compute_commitment(void)
{
    uint8_t board[N * N];
    uint8_t buf[N * N + 16];
    pack_ship_board(board);
    memcpy(buf, board, N * N);
    memcpy(buf + N * N, b.nonce, 16);
    crypto_hash_sha512(b.commit, buf, sizeof(buf));
}

/* Verify a revealed board+nonce hashes to the peer's earlier commitment, and
   that the revealed shots we were told "hit" are actually ships. */
static bool verify_reveal(const uint8_t board[N * N], const uint8_t nonce[16])
{
    uint8_t buf[N * N + 16];
    uint8_t hash[64];
    memcpy(buf, board, N * N);
    memcpy(buf + N * N, nonce, 16);
    crypto_hash_sha512(hash, buf, sizeof(buf));
    if (memcmp(hash, b.peer_commit, 64) != 0) {
        return false;
    }
    /* Cross-check: every cell we recorded as a hit must be a ship in the
       revealed board (catches a peer who answered "hit"/"miss" dishonestly). */
    int ship_count = 0;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            if (board[r * N + c]) { ship_count++; }
            if (b.tgt[r][c] == TGT_HIT && !board[r * N + c]) {
                return false;
            }
        }
    }
    return ship_count == TOTAL_SHIP_CELLS;
}

/* ---- placement --------------------------------------------------------------- */

static bool can_place(int r, int c, int len, bool horiz)
{
    for (int i = 0; i < len; i++) {
        int rr = r + (horiz ? 0 : i);
        int cc = c + (horiz ? i : 0);
        if (rr < 0 || rr >= N || cc < 0 || cc >= N) { return false; }
        if (b.own[rr][cc] != OWN_EMPTY) { return false; }
    }
    return true;
}

static void place(int r, int c, int len, bool horiz)
{
    for (int i = 0; i < len; i++) {
        int rr = r + (horiz ? 0 : i);
        int cc = c + (horiz ? i : 0);
        b.own[rr][cc] = OWN_SHIP;
    }
}

static void draw_own_grid(int cur_r, int cur_c, int len, bool horiz, bool show_cursor)
{
    api->gfx_frame(OWN_X - 1, GRID_Y - 1, N * CELL + 2, N * CELL + 2);
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            int x = OWN_X + c * CELL;
            int y = GRID_Y + r * CELL;
            switch (b.own[r][c]) {
            case OWN_SHIP: api->gfx_rect(x + 1, y + 1, CELL - 2, CELL - 2, true); break;
            case OWN_HIT:  api->gfx_rect(x + 1, y + 1, CELL - 2, CELL - 2, true);
                           api->gfx_px(x + 3, y + 3, false); break;
            case OWN_MISS: api->gfx_px(x + CELL / 2, y + CELL / 2, true); break;
            default: break;
            }
        }
    }
    if (show_cursor) {
        for (int i = 0; i < len; i++) {
            int rr = cur_r + (horiz ? 0 : i);
            int cc = cur_c + (horiz ? i : 0);
            if (rr < N && cc < N) {
                api->gfx_frame(OWN_X + cc * CELL, GRID_Y + rr * CELL, CELL, CELL);
            }
        }
    }
}

static void draw_target_grid(bool show_cursor)
{
    api->gfx_frame(TGT_X - 1, GRID_Y - 1, N * CELL + 2, N * CELL + 2);
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            int x = TGT_X + c * CELL;
            int y = GRID_Y + r * CELL;
            if (b.tgt[r][c] == TGT_HIT) {
                api->gfx_rect(x + 1, y + 1, CELL - 2, CELL - 2, true);
            } else if (b.tgt[r][c] == TGT_MISS) {
                api->gfx_px(x + CELL / 2, y + CELL / 2, true);
            }
        }
    }
    if (show_cursor) {
        api->gfx_frame(TGT_X + b.cur_c * CELL, GRID_Y + b.cur_r * CELL, CELL, CELL);
    }
}

/* Interactive ship placement. Cursor moves with d-pad; OK places at the
   highlighted origin; OK_LONG toggles orientation before placing. */
static bool placement_phase(void)
{
    int cur_r = 0, cur_c = 0;
    bool horiz = true;
    size_t ship_idx = 0;

    while (ship_idx < NUM_SHIPS) {
        int len = k_ships[ship_idx];
        if (cur_r + (horiz ? 0 : len) > N) { cur_r = N - (horiz ? 1 : len); }
        if (cur_c + (horiz ? len : 0) > N) { cur_c = N - (horiz ? len : 1); }
        if (cur_r < 0) { cur_r = 0; }
        if (cur_c < 0) { cur_c = 0; }

        char text[24];
        api->gfx_clear();
        draw_own_grid(cur_r, cur_c, len, horiz, true);
        snprintf(text, sizeof(text), "SHIP %d/%d L%d",
                 (int)ship_idx + 1, (int)NUM_SHIPS, len);
        api->gfx_text(66, 12, text);
        api->gfx_text(66, 22, horiz ? "HORIZ" : "VERT");
        api->gfx_text(66, 34, "OK=PLACE");
        api->gfx_text(66, 42, "HOLD=ROTATE");
        api->gfx_flush();

        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        jpp_sdk_wait_key(s_ctx, 0u, &key);
        switch (key) {
        case JPP_SDK_KEY_UP:    if (cur_r > 0) { cur_r--; } break;
        case JPP_SDK_KEY_DOWN:  cur_r++; break;
        case JPP_SDK_KEY_LEFT:  if (cur_c > 0) { cur_c--; } break;
        case JPP_SDK_KEY_RIGHT: cur_c++; break;
        case JPP_SDK_KEY_OK:
            if (can_place(cur_r, cur_c, len, horiz)) {
                place(cur_r, cur_c, len, horiz);
                api->sfx_tone(880u, 30u);
                ship_idx++;
            } else {
                api->sfx_tone(200u, 80u);
            }
            break;
        case JPP_SDK_KEY_OK_LONG:
            horiz = !horiz;
            break;
        default: break;
        }
    }
    return true;
}

/* ---- helpers ----------------------------------------------------------------- */

static bool all_my_ships_sunk(void)
{
    return b.own_hits_taken >= TOTAL_SHIP_CELLS;
}

/* Answer an incoming shot at (r,c); returns the outcome byte. */
static uint8_t answer_shot(int r, int c)
{
    if (b.own[r][c] == OWN_SHIP) {
        b.own[r][c] = OWN_HIT;
        b.own_hits_taken++;
        return all_my_ships_sunk() ? OUT_WIN : OUT_HIT;
    }
    if (b.own[r][c] == OWN_EMPTY) {
        b.own[r][c] = OWN_MISS;
    }
    return OUT_MISS;
}

static void send_reveal(bool is_host)
{
    uint8_t pkt[1 + 16 + N * N];
    uint8_t board[N * N];
    pkt[0] = MSG_REVEAL;
    memcpy(&pkt[1], b.nonce, 16);
    pack_ship_board(board);
    memcpy(&pkt[17], board, N * N);
    if (is_host) { api->mp_host_send(s_ctx, pkt, sizeof(pkt)); }
    else         { api->mp_client_send(s_ctx, pkt, sizeof(pkt)); }
}

static bool recv_msg(bool is_host, uint8_t *buf, size_t buf_len,
                     size_t *out_len, uint32_t timeout_ms)
{
    if (is_host) {
        return api->mp_host_recv(s_ctx, buf, buf_len, out_len, timeout_ms);
    }
    /* client polls the host TX characteristic until something arrives */
    uint32_t waited = 0u;
    while (waited <= timeout_ms) {
        if (api->mp_client_recv(s_ctx, buf, buf_len, out_len) && *out_len > 0u) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(60u));
        waited += 60u;
    }
    return false;
}

static bool send_msg(bool is_host, const uint8_t *data, size_t len)
{
    return is_host ? api->mp_host_send(s_ctx, data, len)
                   : api->mp_client_send(s_ctx, data, len);
}

/* ---- battle loop ------------------------------------------------------------- */
/*
 * Symmetric turn-based exchange refereed by the host. The host moves first.
 * Each turn: the mover fires a SHOT; the defender replies with a RESULT and,
 * if a hit, marks its own grid. We track strikes (out-of-turn/dup/oob) on the
 * host side. On game end both sides REVEAL and verify.
 */

typedef struct {
    bool is_host;
    bool my_turn;
    uint16_t shot_seq;
    int strikes;
    uint8_t end_reason;
    bool i_won;
    bool peer_revealed;
    uint8_t peer_board[N * N];
    uint8_t peer_nonce[16];
} battle_t;

static void do_reveal_exchange(battle_t *bt)
{
    send_reveal(bt->is_host);
    uint8_t rx[1 + 16 + N * N];
    size_t rx_len = 0u;
    for (int retry = 0; retry < 30 && !bt->peer_revealed; retry++) {
        if (recv_msg(bt->is_host, rx, sizeof(rx), &rx_len, 1000u) &&
            rx_len >= 1u + 16 + N * N && rx[0] == MSG_REVEAL) {
            memcpy(bt->peer_nonce, &rx[1], 16);
            memcpy(bt->peer_board, &rx[17], N * N);
            bt->peer_revealed = true;
        }
    }
}

static void battle_phase(battle_t *bt)
{
    uint8_t rx[1 + 16 + N * N];

    b.cur_r = 0;
    b.cur_c = 0;
    bt->my_turn = bt->is_host;  /* host fires first */

    for (;;) {
        api->gfx_clear();
        draw_own_grid(0, 0, 0, true, false);
        draw_target_grid(bt->my_turn);
        api->gfx_text(2, 58, bt->my_turn ? "FIRE!" : "WAIT...");
        api->gfx_flush();

        if (bt->my_turn) {
            jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
            jpp_sdk_wait_key(s_ctx, 100u, &key);
            bool fire = false;
            switch (key) {
            case JPP_SDK_KEY_UP:    if (b.cur_r > 0) { b.cur_r--; } break;
            case JPP_SDK_KEY_DOWN:  if (b.cur_r < N - 1) { b.cur_r++; } break;
            case JPP_SDK_KEY_LEFT:  if (b.cur_c > 0) { b.cur_c--; } break;
            case JPP_SDK_KEY_RIGHT: if (b.cur_c < N - 1) { b.cur_c++; } break;
            case JPP_SDK_KEY_OK:
                if (b.tgt[b.cur_r][b.cur_c] == TGT_UNKNOWN) { fire = true; }
                break;
            case JPP_SDK_KEY_OK_LONG: {
                uint8_t bye = MSG_BYE;
                send_msg(bt->is_host, &bye, 1u);
                bt->end_reason = END_QUIT;
                return;
            }
            default: break;
            }
            if (!fire) { continue; }

            uint8_t shot[4] = { MSG_SHOT, (uint8_t)++bt->shot_seq,
                                (uint8_t)b.cur_r, (uint8_t)b.cur_c };
            send_msg(bt->is_host, shot, sizeof(shot));

            /* Wait for the RESULT. */
            size_t rx_len = 0u;
            bool got = false;
            for (int retry = 0; retry < 30 && !got; retry++) {
                if (recv_msg(bt->is_host, rx, sizeof(rx), &rx_len, 1000u) &&
                    rx_len >= 5u && rx[0] == MSG_RESULT && rx[1] == bt->shot_seq) {
                    got = true;
                }
            }
            if (!got) { bt->end_reason = END_TIMEOUT; return; }

            uint8_t outcome = rx[4];
            if (outcome == OUT_MISS) {
                b.tgt[b.cur_r][b.cur_c] = TGT_MISS;
            } else {
                b.tgt[b.cur_r][b.cur_c] = TGT_HIT;
                b.enemy_hits++;
                api->sfx_seq(outcome == OUT_WIN ? k_sunk : k_hit,
                             outcome == OUT_WIN ? 4 : 2);
            }
            if (outcome == OUT_WIN) {
                bt->i_won = true;
                do_reveal_exchange(bt);
                return;
            }
            bt->my_turn = false;
        } else {
            /* Defender: wait for the opponent's SHOT, answer it. */
            size_t rx_len = 0u;
            if (!recv_msg(bt->is_host, rx, sizeof(rx), &rx_len, 2000u)) {
                /* allow quitting while waiting */
                jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
                if (jpp_sdk_poll_key(s_ctx, &key) == JPP_SDK_STATUS_OK &&
                    key == JPP_SDK_KEY_OK_LONG) {
                    uint8_t bye = MSG_BYE;
                    send_msg(bt->is_host, &bye, 1u);
                    bt->end_reason = END_QUIT;
                    return;
                }
                continue;
            }
            if (rx_len >= 1u && rx[0] == MSG_BYE) {
                bt->end_reason = END_QUIT;
                return;
            }
            if (rx_len < 4u || rx[0] != MSG_SHOT) { continue; }

            int sr = rx[2];
            int sc = rx[3];
            /* Host referees: out-of-range or already-shot cells are strikes. */
            if (bt->is_host) {
                if (sr < 0 || sr >= N || sc < 0 || sc >= N ||
                    b.own[sr][sc] == OWN_HIT || b.own[sr][sc] == OWN_MISS) {
                    if (++bt->strikes >= AC_THRESHOLD) {
                        bt->end_reason = END_ANTICHEAT;
                        return;
                    }
                }
            }
            if (sr < 0 || sr >= N || sc < 0 || sc >= N) { continue; }

            uint8_t outcome = answer_shot(sr, sc);
            if (outcome != OUT_MISS) {
                api->sfx_seq(outcome == OUT_WIN ? k_lose : k_hit,
                             outcome == OUT_WIN ? 3 : 2);
            }
            uint8_t result[5] = { MSG_RESULT, rx[1], (uint8_t)sr, (uint8_t)sc, outcome };
            send_msg(bt->is_host, result, sizeof(result));

            if (outcome == OUT_WIN) {
                bt->i_won = false;
                do_reveal_exchange(bt);
                return;
            }
            bt->my_turn = true;
        }
    }
}

/* ---- commitment exchange ----------------------------------------------------- */

static bool commit_exchange(bool is_host)
{
    randombytes_buf(b.nonce, sizeof(b.nonce));
    compute_commitment();

    uint8_t pkt[1 + 64];
    pkt[0] = MSG_COMMIT;
    memcpy(&pkt[1], b.commit, 64);
    send_msg(is_host, pkt, sizeof(pkt));

    uint8_t rx[1 + 64];
    size_t rx_len = 0u;
    for (int retry = 0; retry < 50; retry++) {
        if (recv_msg(is_host, rx, sizeof(rx), &rx_len, 1000u) &&
            rx_len >= 1u + 64 && rx[0] == MSG_COMMIT) {
            memcpy(b.peer_commit, &rx[1], 64);
            b.peer_committed = true;
            /* Re-send ours in case the peer missed it before committing. */
            send_msg(is_host, pkt, sizeof(pkt));
            return true;
        }
    }
    return false;
}

void jpp_module_entry(jpp_sdk_context_t *ctx, void *api_ptr)
{
    api = (const games_api_t *)api_ptr;
    if (api == NULL || api->version != GAMES_API_VERSION) {
        return;
    }
    s_ctx = ctx;

    games_mp_role_t role = api->mp_discover(s_ctx, GAMES_MP_ID_BSHIP);
    if (role == GAMES_MP_NONE) {
        return;
    }
    bool is_host = (role == GAMES_MP_HOST);

    memset(&b, 0, sizeof(b));
    placement_phase();

    jpp_sdk_ui_result_t ui;
    if (!commit_exchange(is_host)) {
        api->mp_end(s_ctx);
        jpp_sdk_dialog(s_ctx, "Battleship", "Connection lost during setup.", &ui);
        return;
    }

    battle_t bt = { 0 };
    bt.is_host = is_host;
    battle_phase(&bt);
    api->mp_end(s_ctx);

    if (bt.end_reason == END_ANTICHEAT) {
        api->sfx_seq(k_lose, 3);
        jpp_sdk_dialog(s_ctx, "Session ended",
                       "Illegal shots detected. Session ended.", &ui);
        return;
    }
    if (bt.end_reason == END_QUIT || bt.end_reason == END_TIMEOUT) {
        jpp_sdk_dialog(s_ctx, "Battleship",
                       bt.end_reason == END_TIMEOUT ? "Connection lost."
                                                    : "Match abandoned.", &ui);
        return;
    }

    /* Game finished — verify the loser's reveal proves they didn't cheat. */
    if (bt.peer_revealed && !verify_reveal(bt.peer_board, bt.peer_nonce)) {
        jpp_sdk_dialog(s_ctx, "Cheat detected",
                       "Opponent's board did not match their commitment. "
                       "Result voided.", &ui);
        return;
    }

    char text[48];
    if (bt.i_won) {
        int wins = api->store_get(GAMES_KV_BSHIP_WINS, 0) + 1;
        api->store_set(GAMES_KV_BSHIP_WINS, wins);
        api->sfx_seq(k_win, 4);
        snprintf(text, sizeof(text), "You win! Total wins: %d.", wins);
    } else {
        api->sfx_seq(k_lose, 3);
        snprintf(text, sizeof(text), "You lose. Wins: %d.",
                 api->store_get(GAMES_KV_BSHIP_WINS, 0));
    }
    jpp_sdk_dialog(s_ctx, "Battleship", text, &ui);
}
