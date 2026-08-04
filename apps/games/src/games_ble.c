#include "games_ble.h"
#include "games_gfx.h"
#include "games_rng.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpp_broker_core.h"

/* Manufacturer-specific discovery advertisement (company 0x4A50 — "JP",
   same id meetapp uses):
   [len=8][0xFF][0x50][0x4A]['G'][game_id][PROTO_VER][nonce_lo][nonce_hi]   */
#define GAMES_BLE_COMPANY_LO 0x50u
#define GAMES_BLE_COMPANY_HI 0x4Au
#define GAMES_BLE_MAGIC      0x47u  /* 'G' */
#define GAMES_BLE_PROTO_VER  0x01u
#define GAMES_BLE_AD_LEN     9u

/* Session HELLO the client writes right after connecting:
   ['G']['H'][game_id][PROTO_VER] */
#define GAMES_BLE_HELLO_LEN  4u

#define GAMES_BLE_PKT_MAX    64u    /* largest game packet we ferry */

static games_mp_role_t   s_role = GAMES_MP_NONE;
static jpp_sdk_ble_conn_t s_conn;
static bool              s_advertising;
static bool              s_service_up;

static void build_ad(uint8_t game_id, uint16_t nonce, uint8_t *out)
{
    out[0] = GAMES_BLE_AD_LEN - 1u;  /* length = type byte + 7 data bytes */
    out[1] = 0xFFu;                  /* manufacturer specific */
    out[2] = GAMES_BLE_COMPANY_LO;
    out[3] = GAMES_BLE_COMPANY_HI;
    out[4] = GAMES_BLE_MAGIC;
    out[5] = game_id;
    out[6] = GAMES_BLE_PROTO_VER;
    out[7] = (uint8_t)(nonce & 0xFFu);
    out[8] = (uint8_t)(nonce >> 8);
}

static bool parse_ad(const uint8_t *ad, size_t ad_len, uint8_t game_id)
{
    size_t i = 0u;
    while (i < ad_len) {
        uint8_t len = ad[i];
        if (len == 0u || i + 1u + len > ad_len) {
            break;
        }
        if (ad[i + 1u] == 0xFFu && len >= GAMES_BLE_AD_LEN - 1u &&
            ad[i + 2u] == GAMES_BLE_COMPANY_LO &&
            ad[i + 3u] == GAMES_BLE_COMPANY_HI &&
            ad[i + 4u] == GAMES_BLE_MAGIC &&
            ad[i + 5u] == game_id &&
            ad[i + 6u] == GAMES_BLE_PROTO_VER) {
            return true;
        }
        i += 1u + len;
    }
    return false;
}

static bool is_hello(const uint8_t *buf, size_t len, uint8_t game_id)
{
    return len >= GAMES_BLE_HELLO_LEN &&
           buf[0] == GAMES_BLE_MAGIC && buf[1] == 'H' &&
           buf[2] == game_id && buf[3] == GAMES_BLE_PROTO_VER;
}

/* ---- hex helpers (write_char takes hex; read_char returns hex) ------------- */

static void bin_to_hex(const uint8_t *bin, size_t len, char *out)
{
    for (size_t i = 0u; i < len; i++) {
        snprintf(&out[i * 2u], 3u, "%02X", bin[i]);
    }
    out[len * 2u] = '\0';
}

static size_t hex_to_bin(const char *hex, uint8_t *out, size_t out_max)
{
    size_t len = strlen(hex);
    if (len % 2u != 0u) {
        return 0u;
    }
    size_t bytes = len / 2u;
    if (bytes > out_max) {
        bytes = out_max;
    }
    for (size_t i = 0u; i < bytes; i++) {
        unsigned int byte;
        if (sscanf(&hex[i * 2u], "%02x", &byte) != 1) {
            return 0u;
        }
        out[i] = (uint8_t)byte;
    }
    return bytes;
}

/* ---- discovery -------------------------------------------------------------- */

static void draw_search_screen(int spinner)
{
    static const char *k_frames[4] = { "/", "-", "<", "-" };
    games_gfx_clear();
    games_gfx_text(28, 20, "SEARCHING");
    games_gfx_text(66, 20, k_frames[spinner % 4]);
    games_gfx_text(10, 34, "BRING DEVICES CLOSE");
    games_gfx_text(16, 50, "HOLD OK TO CANCEL");
    games_gfx_flush();
}

static void discovery_cleanup(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t result;
    if (s_advertising) {
        jpp_sdk_ble_advertise_stop(ctx, &result);
        s_advertising = false;
    }
}

games_mp_role_t games_ble_discover(jpp_sdk_context_t *ctx, uint8_t game_id)
{
    jpp_broker_result_t result;
    static jpp_sdk_ble_scan_result_t scan_results[JPP_SDK_BLE_SCAN_MAX];
    static uint8_t rx_buf[GAMES_BLE_PKT_MAX];

    s_role = GAMES_MP_NONE;
    s_conn = JPP_SDK_BLE_CONN_INVALID;
    s_advertising = false;

    /* Register the app-host service (prompts for ble.host on first use) and
       advertise connectable (prompts for ble.advertise). */
    if (jpp_sdk_ble_service_register(ctx, JPP_SDK_BLE_HOST_SVC_UUID,
                                     &result) != JPP_SDK_STATUS_OK) {
        return GAMES_MP_NONE;
    }
    s_service_up = true;
    jpp_sdk_ble_host_clear(ctx, &result);
    if (jpp_sdk_ble_set_connectable(ctx, true, &result) != JPP_SDK_STATUS_OK) {
        games_ble_end(ctx);
        return GAMES_MP_NONE;
    }

    uint8_t ad[GAMES_BLE_AD_LEN];
    build_ad(game_id, (uint16_t)games_rng(0u), ad);

    int spinner = 0;
    for (;;) {
        /* Cancel check between phases. */
        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        while (jpp_sdk_poll_key(ctx, &key) == JPP_SDK_STATUS_OK &&
               key != JPP_SDK_KEY_NONE) {
            if (key == JPP_SDK_KEY_OK_LONG) {
                games_ble_end(ctx);
                return GAMES_MP_NONE;
            }
            key = JPP_SDK_KEY_NONE;
        }
        draw_search_screen(spinner++);

        /* Advertise phase: connectable, waiting for a peer HELLO write. */
        if (jpp_sdk_ble_advertise_start(ctx, ad, sizeof(ad),
                                        &result) != JPP_SDK_STATUS_OK) {
            games_ble_end(ctx);
            return GAMES_MP_NONE;
        }
        s_advertising = true;
        size_t rx_len = sizeof(rx_buf);
        bool received = false;
        uint32_t adv_ms = 600u + games_rng(600u);
        if (jpp_sdk_ble_host_wait_write(ctx, rx_buf, &rx_len, adv_ms,
                                        &received, &result) == JPP_SDK_STATUS_OK &&
            received && is_hello(rx_buf, rx_len, game_id)) {
            discovery_cleanup(ctx);
            s_role = GAMES_MP_HOST;
            return s_role;
        }
        discovery_cleanup(ctx);

        /* Scan phase: look for another device advertising the same game. */
        size_t found = 0u;
        uint32_t scan_ms = 400u + games_rng(400u);
        if (jpp_sdk_ble_scan(ctx, scan_ms, scan_results, JPP_SDK_BLE_SCAN_MAX,
                             &found, &result) != JPP_SDK_STATUS_OK) {
            games_ble_end(ctx);
            return GAMES_MP_NONE;
        }
        for (size_t i = 0u; i < found; i++) {
            if (!parse_ad(scan_results[i].ad_data, scan_results[i].ad_data_len,
                          game_id)) {
                continue;
            }
            /* Found a host candidate — connect (prompts for ble.connect)
               and introduce ourselves. */
            if (jpp_sdk_ble_connect(ctx, scan_results[i].address,
                                    &s_conn) != JPP_SDK_STATUS_OK) {
                continue;
            }
            uint8_t hello[GAMES_BLE_HELLO_LEN] = {
                GAMES_BLE_MAGIC, 'H', game_id, GAMES_BLE_PROTO_VER
            };
            if (!games_ble_client_send(ctx, hello, sizeof(hello))) {
                jpp_sdk_ble_disconnect(ctx, s_conn);
                s_conn = JPP_SDK_BLE_CONN_INVALID;
                continue;
            }
            s_role = GAMES_MP_CLIENT;
            return s_role;
        }
    }
}

/* ---- host transport ---------------------------------------------------------- */

bool games_ble_host_recv(jpp_sdk_context_t *ctx, uint8_t *buf, size_t buf_len,
                         size_t *out_len, uint32_t timeout_ms)
{
    jpp_broker_result_t result;
    bool received = false;
    size_t len = buf_len;

    *out_len = 0u;
    if (s_role != GAMES_MP_HOST) {
        return false;
    }
    if (jpp_sdk_ble_host_wait_write(ctx, buf, &len, timeout_ms,
                                    &received, &result) != JPP_SDK_STATUS_OK ||
        !received) {
        return false;
    }
    *out_len = len;
    return true;
}

bool games_ble_host_send(jpp_sdk_context_t *ctx, const uint8_t *data, size_t len)
{
    jpp_broker_result_t result;
    if (s_role != GAMES_MP_HOST) {
        return false;
    }
    return jpp_sdk_ble_host_set_value(ctx, data, len, &result) == JPP_SDK_STATUS_OK;
}

/* ---- client transport ---------------------------------------------------------- */

bool games_ble_client_send(jpp_sdk_context_t *ctx, const uint8_t *data, size_t len)
{
    jpp_broker_result_t result;
    static char hex[GAMES_BLE_PKT_MAX * 2u + 1u];

    if (s_conn == JPP_SDK_BLE_CONN_INVALID || len > GAMES_BLE_PKT_MAX) {
        return false;
    }
    bin_to_hex(data, len, hex);
    return jpp_sdk_ble_write_char(ctx, s_conn, JPP_SDK_BLE_HOST_SVC_UUID,
                                  JPP_SDK_BLE_HOST_RX_UUID, hex,
                                  &result) == JPP_SDK_STATUS_OK && result.ok;
}

bool games_ble_client_recv(jpp_sdk_context_t *ctx, uint8_t *buf, size_t buf_len,
                           size_t *out_len)
{
    jpp_broker_result_t result;

    *out_len = 0u;
    if (s_conn == JPP_SDK_BLE_CONN_INVALID) {
        return false;
    }
    if (jpp_sdk_ble_read_char(ctx, s_conn, JPP_SDK_BLE_HOST_SVC_UUID,
                              JPP_SDK_BLE_HOST_TX_UUID,
                              &result) != JPP_SDK_STATUS_OK || !result.ok) {
        return false;
    }
    const char *hex = jpp_broker_result_get(&result, "text");
    if (hex == NULL) {
        return false;
    }
    *out_len = hex_to_bin(hex, buf, buf_len);
    return *out_len > 0u;
}

/* ---- teardown ------------------------------------------------------------------ */

void games_ble_end(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t result;

    if (s_conn != JPP_SDK_BLE_CONN_INVALID) {
        jpp_sdk_ble_disconnect(ctx, s_conn);
        s_conn = JPP_SDK_BLE_CONN_INVALID;
    }
    discovery_cleanup(ctx);
    if (s_service_up) {
        jpp_sdk_ble_host_clear(ctx, &result);
        jpp_sdk_ble_service_unregister(ctx, &result);
        s_service_up = false;
    }
    s_role = GAMES_MP_NONE;
}
