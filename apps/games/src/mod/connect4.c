/*
 * Connect-4 — Games-app module. BLE two-device only.
 *
 * 7×6 board, 8×8 px cells. LEFT/RIGHT move the column cursor, CENTER drops a
 * disc. The match is turn-based over the hub's BLE session, so the slow BLE
 * round-trip never hurts gameplay. The host (advertiser) is the authority:
 * it owns the board, validates every client move (your turn, column not
 * full, in range), and ends the session after 3 illegal/out-of-turn attempts
 * — a basic anti-cheat for a turn-based game. Wins are tallied across the
 * session into GAMES_KV_C4_WINS.
 *
 * Wire (raw bytes, ferried by the hub; client side hex-encoded by the SDK):
 *   client→host MOVE   : ['C'][seq][column]
 *   client→host BYE    : ['C'][seq][0xFF]
 *   host→client STATE  : ['c'][board 42 bytes][turn][winner][end_reason]
 * Sounds: drop zip · your-turn blip · win fanfare · lose run.
 */

#include "games_api.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const games_api_t *api;
static jpp_sdk_context_t *s_ctx;

#define COLS 7
#define ROWS 6
#define CELL 8
#define BOARD_X 36           /* centered: 36 + 7*8 = 92 */
#define BOARD_Y 8

#define MOVE_MAGIC  'C'
#define STATE_MAGIC 'c'
#define MOVE_BYE    0xFFu

#define TURN_HOST   0u       /* host plays disc value 1 */
#define TURN_CLIENT 1u       /* client plays disc value 2 */

#define WIN_NONE    0u
#define WIN_HOST    1u
#define WIN_CLIENT  2u
#define WIN_DRAW    3u

#define END_NONE      0u
#define END_QUIT      1u
#define END_ANTICHEAT 2u
#define END_TIMEOUT   3u

#define AC_THRESHOLD  3
#define TURN_TIMEOUT_MS 30000u

typedef struct {
    uint8_t cell[ROWS][COLS]; /* 0 empty, 1 host, 2 client */
    uint8_t turn;             /* TURN_HOST / TURN_CLIENT */
    uint8_t winner;           /* WIN_* */
} c4_board_t;

static c4_board_t b;
static int s_cursor;

static const jpp_buzzer_note_t k_drop[]  = { {880,20},{700,20},{550,20},{440,30} };
static const jpp_buzzer_note_t k_win[]   = { {660,80},{880,80},{1100,80},{1320,160} };
static const jpp_buzzer_note_t k_lose[]  = { {440,120},{330,120},{220,200} };

/* ---- rules -------------------------------------------------------------------- */

static int column_height(int col)
{
    int h = 0;
    for (int r = 0; r < ROWS; r++) {
        if (b.cell[r][col]) { h++; }
    }
    return h;
}

static bool drop_disc(int col, uint8_t player, int *out_row)
{
    if (col < 0 || col >= COLS || column_height(col) >= ROWS) {
        return false;
    }
    int row = ROWS - 1 - column_height(col);
    b.cell[row][col] = player;
    if (out_row) { *out_row = row; }
    return true;
}

static bool wins_at(int row, int col, uint8_t player)
{
    static const int dirs[4][2] = { {0,1},{1,0},{1,1},{1,-1} };
    for (int d = 0; d < 4; d++) {
        int count = 1;
        for (int s = -1; s <= 1; s += 2) {
            int r = row + dirs[d][0] * s;
            int c = col + dirs[d][1] * s;
            while (r >= 0 && r < ROWS && c >= 0 && c < COLS &&
                   b.cell[r][c] == player) {
                count++;
                r += dirs[d][0] * s;
                c += dirs[d][1] * s;
            }
        }
        if (count >= 4) { return true; }
    }
    return false;
}

static bool board_full(void)
{
    for (int c = 0; c < COLS; c++) {
        if (column_height(c) < ROWS) { return false; }
    }
    return true;
}

/* Applies a validated move for `player`; updates turn/winner. */
static void apply_move(int col, uint8_t player)
{
    int row = 0;
    if (!drop_disc(col, player, &row)) { return; }
    if (wins_at(row, col, player)) {
        b.winner = (player == 1u) ? WIN_HOST : WIN_CLIENT;
    } else if (board_full()) {
        b.winner = WIN_DRAW;
    } else {
        b.turn = (b.turn == TURN_HOST) ? TURN_CLIENT : TURN_HOST;
    }
}

/* ---- rendering ----------------------------------------------------------------- */

static void render(bool we_are_host, bool our_turn)
{
    char text[24];

    api->gfx_clear();
    api->gfx_frame(BOARD_X - 2, BOARD_Y - 2, COLS * CELL + 3, ROWS * CELL + 3);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int x = BOARD_X + c * CELL;
            int y = BOARD_Y + r * CELL;
            uint8_t v = b.cell[r][c];
            if (v == 1u) {
                api->gfx_rect(x + 1, y + 1, CELL - 3, CELL - 3, true);  /* host: solid */
            } else if (v == 2u) {
                api->gfx_frame(x + 1, y + 1, CELL - 3, CELL - 3);       /* client: ring */
            } else {
                api->gfx_px(x + CELL / 2 - 1, y + CELL / 2 - 1, true);
            }
        }
    }
    /* cursor arrow above the active column */
    if (our_turn && b.winner == WIN_NONE) {
        int cx = BOARD_X + s_cursor * CELL + CELL / 2;
        api->gfx_text(cx - 1, BOARD_Y - 8, "v");
    }
    snprintf(text, sizeof(text), "%s",
             b.winner != WIN_NONE ? "" : (our_turn ? "YOUR TURN" : "WAIT..."));
    api->gfx_text(2, 2, we_are_host ? "YOU=#" : "YOU=O");
    api->gfx_text(2, 54, text);
    api->gfx_flush();
}

/* ---- host loop ----------------------------------------------------------------- */

static void host_send_state(uint8_t end_reason)
{
    uint8_t pkt[1 + ROWS * COLS + 3];
    pkt[0] = STATE_MAGIC;
    memcpy(&pkt[1], b.cell, ROWS * COLS);
    pkt[1 + ROWS * COLS] = b.turn;
    pkt[2 + ROWS * COLS] = b.winner;
    pkt[3 + ROWS * COLS] = end_reason;
    api->mp_host_send(s_ctx, pkt, sizeof(pkt));
}

/* returns end reason */
static uint8_t play_host(void)
{
    uint8_t rx[16];
    int violations = 0;
    uint16_t last_seq = 0u;
    bool have_seq = false;

    memset(&b, 0, sizeof(b));
    b.turn = (api->rng(2u) ? TURN_HOST : TURN_CLIENT);
    s_cursor = 3;
    host_send_state(END_NONE);

    for (;;) {
        bool our_turn = (b.turn == TURN_HOST) && (b.winner == WIN_NONE);
        render(true, our_turn);
        if (b.winner != WIN_NONE) {
            host_send_state(END_NONE);
            return END_NONE;
        }

        if (our_turn) {
            jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
            jpp_sdk_wait_key(s_ctx, 100u, &key);
            switch (key) {
            case JPP_SDK_KEY_LEFT:
                if (s_cursor > 0) { s_cursor--; }
                break;
            case JPP_SDK_KEY_RIGHT:
                if (s_cursor < COLS - 1) { s_cursor++; }
                break;
            case JPP_SDK_KEY_CENTER:
                if (column_height(s_cursor) < ROWS) {
                    api->sfx_seq(k_drop, 4);
                    apply_move(s_cursor, 1u);
                    host_send_state(END_NONE);
                }
                break;
            case JPP_SDK_KEY_CENTER_LONG:
                host_send_state(END_QUIT);
                return END_QUIT;
            default: break;
            }
        } else {
            /* Wait for the client's move. */
            size_t rx_len = 0u;
            if (!api->mp_host_recv(s_ctx, rx, sizeof(rx), &rx_len, 2000u)) {
                jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
                if (jpp_sdk_poll_key(s_ctx, &key) == JPP_SDK_STATUS_OK &&
                    key == JPP_SDK_KEY_CENTER_LONG) {
                    host_send_state(END_QUIT);
                    return END_QUIT;
                }
                continue;  /* keep waiting; client is on a slow link */
            }
            if (rx_len < 3u || rx[0] != MOVE_MAGIC) {
                continue;
            }
            uint16_t seq = rx[1];
            if (have_seq && seq == last_seq) {
                continue;  /* duplicate retransmit */
            }
            if (rx[2] == MOVE_BYE) {
                return END_QUIT;
            }
            /* Validate: must be the client's turn, column in range and open. */
            int col = (int)rx[2];
            if (col < 0 || col >= COLS || column_height(col) >= ROWS) {
                if (++violations >= AC_THRESHOLD) {
                    host_send_state(END_ANTICHEAT);
                    return END_ANTICHEAT;
                }
                host_send_state(END_NONE);  /* nudge client to resync */
                continue;
            }
            have_seq = true;
            last_seq = seq;
            api->sfx_seq(k_drop, 4);
            apply_move(col, 2u);
            host_send_state(END_NONE);
        }
    }
}

/* ---- client loop --------------------------------------------------------------- */

static bool client_send_move(uint16_t seq, uint8_t col)
{
    uint8_t pkt[3] = { MOVE_MAGIC, (uint8_t)seq, col };
    return api->mp_client_send(s_ctx, pkt, sizeof(pkt));
}

static uint8_t play_client(void)
{
    uint8_t rx[64];
    uint16_t seq = 0u;
    int stale = 0;

    memset(&b, 0, sizeof(b));
    s_cursor = 3;

    for (;;) {
        bool our_turn = (b.turn == TURN_CLIENT) && (b.winner == WIN_NONE);
        render(false, our_turn);
        if (b.winner != WIN_NONE) {
            return END_NONE;
        }

        if (our_turn) {
            jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
            jpp_sdk_wait_key(s_ctx, 100u, &key);
            uint8_t move_col = 0xFEu;
            switch (key) {
            case JPP_SDK_KEY_LEFT:
                if (s_cursor > 0) { s_cursor--; }
                break;
            case JPP_SDK_KEY_RIGHT:
                if (s_cursor < COLS - 1) { s_cursor++; }
                break;
            case JPP_SDK_KEY_CENTER:
                if (column_height(s_cursor) < ROWS) { move_col = (uint8_t)s_cursor; }
                break;
            case JPP_SDK_KEY_CENTER_LONG:
                client_send_move(++seq, MOVE_BYE);
                return END_QUIT;
            default: break;
            }
            if (move_col != 0xFEu) {
                api->sfx_seq(k_drop, 4);
                /* Send the move, then poll for the updated state. */
                client_send_move(++seq, move_col);
                for (int retry = 0; retry < 20; retry++) {
                    size_t rx_len = 0u;
                    if (api->mp_client_recv(s_ctx, rx, sizeof(rx), &rx_len) &&
                        rx_len >= 1u + ROWS * COLS + 3 && rx[0] == STATE_MAGIC) {
                        memcpy(b.cell, &rx[1], ROWS * COLS);
                        b.turn   = rx[1 + ROWS * COLS];
                        b.winner = rx[2 + ROWS * COLS];
                        uint8_t end_reason = rx[3 + ROWS * COLS];
                        if (end_reason != END_NONE) { return end_reason; }
                        if (b.turn == TURN_CLIENT && b.winner == WIN_NONE) {
                            /* host rejected our move (still our turn) */
                            break;
                        }
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(50u));
                }
            }
        } else {
            /* Poll for the host's state until it's our turn again. */
            size_t rx_len = 0u;
            if (api->mp_client_recv(s_ctx, rx, sizeof(rx), &rx_len) &&
                rx_len >= 1u + ROWS * COLS + 3 && rx[0] == STATE_MAGIC) {
                memcpy(b.cell, &rx[1], ROWS * COLS);
                b.turn   = rx[1 + ROWS * COLS];
                b.winner = rx[2 + ROWS * COLS];
                uint8_t end_reason = rx[3 + ROWS * COLS];
                if (end_reason != END_NONE) { return end_reason; }
                stale = 0;
            } else if (++stale > 60) {
                return END_TIMEOUT;
            }
            jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
            if (jpp_sdk_poll_key(s_ctx, &key) == JPP_SDK_STATUS_OK &&
                key == JPP_SDK_KEY_CENTER_LONG) {
                client_send_move(++seq, MOVE_BYE);
                return END_QUIT;
            }
            vTaskDelay(pdMS_TO_TICKS(150u));
        }
    }
}

void jpp_module_entry(jpp_sdk_context_t *ctx, void *api_ptr)
{
    api = (const games_api_t *)api_ptr;
    if (api == NULL || api->version != GAMES_API_VERSION) {
        return;
    }
    s_ctx = ctx;

    games_mp_role_t role = api->mp_discover(s_ctx, GAMES_MP_ID_C4);
    if (role == GAMES_MP_NONE) {
        return;
    }

    bool we_are_host = (role == GAMES_MP_HOST);
    uint8_t end_reason = we_are_host ? play_host() : play_client();
    api->mp_end(s_ctx);

    jpp_sdk_ui_result_t ui;
    if (end_reason == END_ANTICHEAT) {
        api->sfx_seq(k_lose, 3);
        jpp_sdk_dialog(s_ctx, "Session ended",
                       we_are_host ? "Illegal moves detected. Session ended."
                                   : "The host ended the session.", &ui);
        return;
    }
    if (end_reason == END_TIMEOUT || end_reason == END_QUIT) {
        jpp_sdk_dialog(s_ctx, "Connect-4",
                       end_reason == END_TIMEOUT ? "Connection lost."
                                                 : "Match abandoned.", &ui);
        return;
    }

    bool we_won = (we_are_host && b.winner == WIN_HOST) ||
                  (!we_are_host && b.winner == WIN_CLIENT);
    bool draw = (b.winner == WIN_DRAW);
    char text[48];
    if (draw) {
        snprintf(text, sizeof(text), "It's a draw. Wins: %d.",
                 api->store_get(GAMES_KV_C4_WINS, 0));
    } else if (we_won) {
        int wins = api->store_get(GAMES_KV_C4_WINS, 0) + 1;
        api->store_set(GAMES_KV_C4_WINS, wins);
        api->sfx_seq(k_win, 4);
        snprintf(text, sizeof(text), "You win! Total wins: %d.", wins);
    } else {
        api->sfx_seq(k_lose, 3);
        snprintf(text, sizeof(text), "You lose. Wins: %d.",
                 api->store_get(GAMES_KV_C4_WINS, 0));
    }
    jpp_sdk_dialog(s_ctx, "Connect-4", text, &ui);
}
