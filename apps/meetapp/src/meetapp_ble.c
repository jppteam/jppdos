#include "meetapp_ble.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpp_broker_core.h"

static const char *TAG = "meetapp_ble";

/* ---- Advertisement builder ----------------------------------------------- */

void meetapp_ble_build_ad(uint8_t type,
                           const uint8_t session_nonce[8],
                           const char *nickname,
                           uint8_t *out_ad, size_t *out_len)
{
    uint8_t nk_len = (uint8_t)strnlen(nickname, MEETAPP_NICKNAME_MAX);
    /* AD structure: [length][0xFF][company_lo][company_hi][type][nonce[8]][nk_len][nk...] */
    uint8_t ad_data_len = (uint8_t)(2u + 1u + 8u + 1u + nk_len); /* company(2)+type(1)+nonce(8)+nklen(1)+nk */
    out_ad[0] = 1u + ad_data_len; /* length field = type byte + data */
    out_ad[1] = 0xFFu;            /* manufacturer specific */
    out_ad[2] = MEETAPP_COMPANY_ID_LO;
    out_ad[3] = MEETAPP_COMPANY_ID_HI;
    out_ad[4] = type;
    memcpy(&out_ad[5], session_nonce, 8u);
    out_ad[13] = nk_len;
    memcpy(&out_ad[14], nickname, nk_len);
    *out_len = 14u + nk_len;
}

/* ---- Parse a raw AD blob for a meetapp manufacturer record --------------- */

static bool parse_meetapp_ad(const uint8_t *ad, size_t ad_len,
                              uint8_t *out_type,
                              uint8_t out_nonce[8],
                              char *out_nickname)
{
    size_t i = 0u;
    while (i < ad_len) {
        uint8_t len  = ad[i];
        if (len == 0u || i + 1u + len > ad_len) break;
        uint8_t type = ad[i + 1u];
        if (type == 0xFFu && len >= 12u) {
            /* Company ID check. */
            if (ad[i + 2u] == MEETAPP_COMPANY_ID_LO &&
                ad[i + 3u] == MEETAPP_COMPANY_ID_HI) {
                *out_type = ad[i + 4u];
                memcpy(out_nonce, &ad[i + 5u], 8u);
                uint8_t nk_len = ad[i + 13u];
                if (nk_len > MEETAPP_NICKNAME_MAX) nk_len = MEETAPP_NICKNAME_MAX;
                if (out_nickname) {
                    memcpy(out_nickname, &ad[i + 14u], nk_len);
                    out_nickname[nk_len] = '\0';
                }
                return true;
            }
        }
        i += 1u + len;
    }
    return false;
}

/* ---- Scan for PONGs ------------------------------------------------------ */

size_t meetapp_ble_scan_pongs(jpp_sdk_context_t *ctx,
                               meetapp_session_t *session,
                               uint32_t scan_ms)
{
    static jpp_sdk_ble_scan_result_t scan_results[JPP_SDK_BLE_SCAN_MAX];
    size_t found = 0u;
    jpp_broker_result_t result;

    jpp_sdk_status_t st = jpp_sdk_ble_scan(ctx, scan_ms, scan_results,
                                            JPP_SDK_BLE_SCAN_MAX, &found, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) return 0u;

    size_t new_peers = 0u;
    for (size_t i = 0u; i < found; i++) {
        uint8_t ad_type;
        uint8_t nonce[8];
        char nickname[MEETAPP_NICKNAME_MAX + 1u];

        if (!parse_meetapp_ad(scan_results[i].ad_data,
                              scan_results[i].ad_data_len,
                              &ad_type, nonce, nickname)) continue;

        if (ad_type != MEETAPP_AD_PONG) continue;
        if (memcmp(nonce, session->session_nonce, 8u) != 0) continue;

        /* Check if already in the list. */
        bool dup = false;
        for (size_t j = 0u; j < session->peer_count; j++) {
            if (strcmp(session->peers[j].ble_addr, scan_results[i].address) == 0) {
                dup = true; break;
            }
        }
        if (dup) continue;
        if (session->peer_count >= MEETAPP_MAX_PEERS) continue;

        meetapp_peer_t *p = &session->peers[session->peer_count++];
        strncpy(p->nickname, nickname, MEETAPP_NICKNAME_MAX);
        p->nickname[MEETAPP_NICKNAME_MAX] = '\0';
        strncpy(p->ble_addr, scan_results[i].address, JPP_SDK_BLE_ADDR_LEN - 1u);
        p->ble_addr[JPP_SDK_BLE_ADDR_LEN - 1u] = '\0';
        new_peers++;

        ESP_LOGI(TAG, "PONG from %s nick=%s", p->ble_addr, p->nickname);
    }
    return new_peers;
}

/* ---- Hex decode helper --------------------------------------------------- */

static size_t hex_to_bin(const char *hex, uint8_t *out, size_t out_max)
{
    size_t len = strlen(hex);
    if (len % 2u != 0u) return 0u;
    size_t bytes = len / 2u;
    if (bytes > out_max) bytes = out_max;
    for (size_t i = 0u; i < bytes; i++) {
        unsigned int byte;
        if (sscanf(&hex[i * 2u], "%02x", &byte) != 1) return 0u;
        out[i] = (uint8_t)byte;
    }
    return bytes;
}

/* ---- Hex encode helper --------------------------------------------------- */

static void bin_to_hex(const uint8_t *bin, size_t len, char *out)
{
    for (size_t i = 0u; i < len; i++) {
        snprintf(&out[i * 2u], 3u, "%02X", bin[i]);
    }
    out[len * 2u] = '\0';
}

/* ---- Leader round A: collect pubkeys and nonces -------------------------- */

bool meetapp_ble_collect_nonces(jpp_sdk_context_t *ctx,
                                 const meetapp_identity_t *identity,
                                 meetapp_session_t *session)
{
    jpp_broker_result_t result;

    for (size_t i = 0u; i < session->peer_count; i++) {
        meetapp_peer_t *p = &session->peers[i];
        ESP_LOGI(TAG, "Round A: connecting to %s", p->ble_addr);

        /* Connect. */
        jpp_sdk_ble_conn_t conn;
        jpp_sdk_status_t st = jpp_sdk_ble_connect(ctx, p->ble_addr, &conn);
        if (st != JPP_SDK_STATUS_OK) {
            ESP_LOGW(TAG, "Connect failed: %s", p->ble_addr);
            continue;
        }

        /* Read TX char: [MEETAPP_TX_NONCE][pubkey[32]][pubnonce[64]] = 97 bytes = 194 hex. */
        st = jpp_sdk_ble_read_char(ctx, conn, JPP_SDK_BLE_HOST_SVC_UUID, JPP_SDK_BLE_HOST_TX_UUID, &result);
        if (st == JPP_SDK_STATUS_OK && result.ok) {
            const char *hex = jpp_broker_result_get(&result, "text");
            if (hex != NULL && strlen(hex) >= 194u) {
                uint8_t raw[97];
                size_t n = hex_to_bin(hex, raw, sizeof(raw));
                if (n >= 97u && raw[0] == MEETAPP_TX_NONCE) {
                    memcpy(p->pubkey,   &raw[1],  32u);
                    memcpy(p->pubnonce, &raw[33], 64u);
                }
            }
        }

        jpp_sdk_ble_disconnect(ctx, conn);
        vTaskDelay(pdMS_TO_TICKS(100u));
    }
    return true;
}

/* ---- Leader round B: send round-1.5 and collect partial sigs ------------- */

bool meetapp_ble_exchange_sigs(jpp_sdk_context_t *ctx,
                                meetapp_session_t *session,
                                const uint8_t msg_hash[64],
                                const uint8_t agg_pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES],
                                const uint8_t *all_pubkeys,
                                size_t n_total)
{
    jpp_broker_result_t result;

    /* Build round-1.5 binary payload:
       [MEETAPP_RX_ROUND15][msg_hash[64]][agg_pubnonce[64]][n[1]][pubkeys[n*32]] */
    size_t payload_len = 1u + 64u + 64u + 1u + n_total * 32u;
    static uint8_t r15_bin[1u + 64u + 64u + 1u + MEETAPP_MAX_TOTAL * 32u];
    r15_bin[0] = MEETAPP_RX_ROUND15;
    memcpy(&r15_bin[1],    msg_hash,    64u);
    memcpy(&r15_bin[65],   agg_pubnonce, 64u);
    r15_bin[129] = (uint8_t)n_total;
    memcpy(&r15_bin[130],  all_pubkeys, n_total * 32u);

    static char r15_hex[2u * (1u + 64u + 64u + 1u + MEETAPP_MAX_TOTAL * 32u) + 1u];
    bin_to_hex(r15_bin, payload_len, r15_hex);

    for (size_t i = 0u; i < session->peer_count; i++) {
        meetapp_peer_t *p = &session->peers[i];
        ESP_LOGI(TAG, "Round B: connecting to %s", p->ble_addr);

        jpp_sdk_ble_conn_t conn;
        if (jpp_sdk_ble_connect(ctx, p->ble_addr, &conn) != JPP_SDK_STATUS_OK) {
            ESP_LOGW(TAG, "Connect failed: %s", p->ble_addr);
            continue;
        }

        /* Write round-1.5 to RX char. */
        jpp_sdk_ble_write_char(ctx, conn, JPP_SDK_BLE_HOST_SVC_UUID, JPP_SDK_BLE_HOST_RX_UUID,
                               r15_hex, &result);

        /* Give participant time to compute partial sig (~500ms). */
        vTaskDelay(pdMS_TO_TICKS(800u));

        /* Read TX char: [MEETAPP_TX_PARTSIG][partial_sig[32]] = 33 bytes = 66 hex. */
        if (jpp_sdk_ble_read_char(ctx, conn, JPP_SDK_BLE_HOST_SVC_UUID, JPP_SDK_BLE_HOST_TX_UUID,
                                  &result) == JPP_SDK_STATUS_OK && result.ok) {
            const char *hex = jpp_broker_result_get(&result, "text");
            if (hex != NULL && strlen(hex) >= 66u) {
                uint8_t raw[33];
                if (hex_to_bin(hex, raw, sizeof(raw)) >= 33u &&
                    raw[0] == MEETAPP_TX_PARTSIG) {
                    memcpy(p->partial_sig, &raw[1], 32u);
                    ESP_LOGI(TAG, "Got partial sig from %s", p->ble_addr);
                }
            }
        }

        jpp_sdk_ble_disconnect(ctx, conn);
        vTaskDelay(pdMS_TO_TICKS(100u));
    }
    return true;
}

/* ---- Leader round C: distribute final signature -------------------------- */

bool meetapp_ble_distribute_sig(jpp_sdk_context_t *ctx,
                                 meetapp_session_t *session,
                                 const uint8_t final_sig[JPP_CRYPTO_SIG_BYTES])
{
    jpp_broker_result_t result;

    /* Build payload: [MEETAPP_RX_FINALSIG][sig[64]] = 65 bytes = 130 hex. */
    uint8_t payload[65];
    payload[0] = MEETAPP_RX_FINALSIG;
    memcpy(&payload[1], final_sig, 64u);

    static char payload_hex[131];
    bin_to_hex(payload, 65u, payload_hex);

    for (size_t i = 0u; i < session->peer_count; i++) {
        meetapp_peer_t *p = &session->peers[i];
        ESP_LOGI(TAG, "Round C: connecting to %s", p->ble_addr);

        jpp_sdk_ble_conn_t conn;
        if (jpp_sdk_ble_connect(ctx, p->ble_addr, &conn) != JPP_SDK_STATUS_OK) {
            ESP_LOGW(TAG, "Connect failed: %s", p->ble_addr);
            continue;
        }

        jpp_sdk_ble_write_char(ctx, conn, JPP_SDK_BLE_HOST_SVC_UUID, JPP_SDK_BLE_HOST_RX_UUID,
                               payload_hex, &result);
        jpp_sdk_ble_disconnect(ctx, conn);
        vTaskDelay(pdMS_TO_TICKS(100u));
    }
    return true;
}

/* ---- Participant: wait for PING ------------------------------------------ */

bool meetapp_ble_scan_ping_once(jpp_sdk_context_t *ctx,
                                 uint8_t session_nonce[8],
                                 char leader_addr[JPP_SDK_BLE_ADDR_LEN],
                                 uint32_t scan_ms)
{
    static jpp_sdk_ble_scan_result_t scan_results[JPP_SDK_BLE_SCAN_MAX];
    size_t found = 0u;
    jpp_broker_result_t result;

    if (jpp_sdk_ble_scan(ctx, scan_ms, scan_results, JPP_SDK_BLE_SCAN_MAX,
                         &found, &result) != JPP_SDK_STATUS_OK) {
        return false;
    }

    for (size_t i = 0u; i < found; i++) {
        uint8_t ad_type;
        uint8_t nonce[8];
        char nickname[MEETAPP_NICKNAME_MAX + 1u];

        if (!parse_meetapp_ad(scan_results[i].ad_data,
                              scan_results[i].ad_data_len,
                              &ad_type, nonce, nickname)) continue;
        if (ad_type != MEETAPP_AD_PING) continue;

        memcpy(session_nonce, nonce, 8u);
        strncpy(leader_addr, scan_results[i].address, JPP_SDK_BLE_ADDR_LEN - 1u);
        leader_addr[JPP_SDK_BLE_ADDR_LEN - 1u] = '\0';
        return true;
    }
    return false;
}

/* ---- Participant: wait for round-1.5 data -------------------------------- */

bool meetapp_ble_wait_round15(jpp_sdk_context_t *ctx,
                               uint8_t msg_hash[64],
                               uint8_t agg_pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES],
                               uint8_t *all_pubkeys,
                               size_t *n_total,
                               uint32_t timeout_ms)
{
    static uint8_t rx_buf[JPP_SDK_BLE_HOST_RX_MAX];
    size_t rx_len = sizeof(rx_buf);
    bool received = false;
    jpp_broker_result_t result;

    if (jpp_sdk_ble_host_wait_write(ctx, rx_buf, &rx_len, timeout_ms,
                                    &received, &result) != JPP_SDK_STATUS_OK ||
        !received) {
        return false;
    }

    /* Expected: [MEETAPP_RX_ROUND15][msg_hash[64]][agg_pubnonce[64]][n][pubkeys] */
    if (rx_len < 130u || rx_buf[0] != MEETAPP_RX_ROUND15) return false;

    size_t n = rx_buf[129];
    if (n == 0u || n > MEETAPP_MAX_TOTAL) return false;
    if (rx_len < 130u + n * 32u) return false;

    memcpy(msg_hash,    &rx_buf[1],   64u);
    memcpy(agg_pubnonce, &rx_buf[65], 64u);
    *n_total = n;
    memcpy(all_pubkeys, &rx_buf[130], n * 32u);
    return true;
}

/* ---- Participant: wait for final signature -------------------------------- */

bool meetapp_ble_wait_final_sig(jpp_sdk_context_t *ctx,
                                 uint8_t final_sig[JPP_CRYPTO_SIG_BYTES],
                                 uint32_t timeout_ms)
{
    static uint8_t rx_buf[JPP_SDK_BLE_HOST_RX_MAX];
    size_t rx_len = sizeof(rx_buf);
    bool received = false;
    jpp_broker_result_t result;

    if (jpp_sdk_ble_host_wait_write(ctx, rx_buf, &rx_len, timeout_ms,
                                    &received, &result) != JPP_SDK_STATUS_OK ||
        !received) {
        return false;
    }

    /* Expected: [MEETAPP_RX_FINALSIG][sig[64]] */
    if (rx_len < 65u || rx_buf[0] != MEETAPP_RX_FINALSIG) return false;

    memcpy(final_sig, &rx_buf[1], 64u);
    return true;
}
