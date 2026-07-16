#include "jpp_ble_msg.h"

#include <string.h>
#include <stdio.h>

#include "jpp_broker_core.h"

/* ---- Frame constants ----------------------------------------------------- */

#define MSG_FRAME_BEGIN 0x01u
#define MSG_FRAME_DATA  0x02u
#define MSG_BEGIN_LEN   9u   /* type(1) + total_len(4) + crc(4) */
#define MSG_DATA_HDR    3u   /* type(1) + seq(2)               */

/* Per-chunk wait once a transfer is in progress. Acknowledged writes arrive one
   connection interval apart (tens of ms), so a few seconds is generous while
   still bounding the hang if the sender dies mid-message. */
#define MSG_CHUNK_TIMEOUT_MS 5000u

/* ---- Little-endian helpers ----------------------------------------------- */

static void wr_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* CRC-32/ISO-HDLC (zlib crc32) — matches JPPD-SMP's crc32_buf so host tooling
   can verify. Bitwise so it needs no table. */
static uint32_t crc32_buf(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---- Sender -------------------------------------------------------------- */

/* Hex-encode one binary frame and write it to the peer's RX characteristic.
   jpp_sdk_ble_write_char takes hex text and decodes it to binary on the wire;
   it uses an acknowledged write (single ATT Write Request, or Prepare/Execute
   above MTU-3), which is what paces the transfer. */
static jpp_ble_msg_result_t write_frame(jpp_sdk_context_t *ctx,
                                        jpp_sdk_ble_conn_t conn,
                                        const char *svc_uuid,
                                        const char *char_uuid,
                                        const uint8_t *frame, size_t n)
{
    /* Static (BLE ops are serialized by the broker's exclusive lock, so this is
       not reentered) to keep this off the app task stack. */
    static char hex[2u * (MSG_DATA_HDR + JPP_BLE_MSG_CHUNK_MAX) + 1u];
    for (size_t i = 0u; i < n; i++) {
        snprintf(&hex[i * 2u], 3u, "%02X", frame[i]);
    }
    hex[n * 2u] = '\0';

    jpp_broker_result_t result;
    jpp_sdk_status_t st = jpp_sdk_ble_write_char(ctx, conn, svc_uuid, char_uuid,
                                                 hex, &result);
    return (st == JPP_SDK_STATUS_OK && result.ok) ? JPP_BLE_MSG_OK
                                                   : JPP_BLE_MSG_ERR_WRITE;
}

jpp_ble_msg_result_t jpp_ble_msg_send(jpp_sdk_context_t *ctx,
                                      jpp_sdk_ble_conn_t conn,
                                      const char *svc_uuid,
                                      const char *char_uuid,
                                      const uint8_t *data,
                                      size_t len)
{
    if (ctx == NULL || svc_uuid == NULL || char_uuid == NULL ||
        (data == NULL && len > 0u)) {
        return JPP_BLE_MSG_ERR_ARG;
    }

    uint8_t begin[MSG_BEGIN_LEN];
    begin[0] = MSG_FRAME_BEGIN;
    wr_u32le(&begin[1], (uint32_t)len);
    wr_u32le(&begin[5], crc32_buf(data, len));
    jpp_ble_msg_result_t r = write_frame(ctx, conn, svc_uuid, char_uuid,
                                         begin, MSG_BEGIN_LEN);
    if (r != JPP_BLE_MSG_OK) {
        return r;
    }

    static uint8_t frame[MSG_DATA_HDR + JPP_BLE_MSG_CHUNK_MAX];
    uint16_t seq = 0u;
    size_t   off = 0u;
    while (off < len) {
        size_t n = len - off;
        if (n > JPP_BLE_MSG_CHUNK_MAX) {
            n = JPP_BLE_MSG_CHUNK_MAX;
        }
        frame[0] = MSG_FRAME_DATA;
        wr_u16le(&frame[1], seq);
        memcpy(&frame[MSG_DATA_HDR], data + off, n);
        r = write_frame(ctx, conn, svc_uuid, char_uuid, frame, MSG_DATA_HDR + n);
        if (r != JPP_BLE_MSG_OK) {
            return r;
        }
        off += n;
        seq++;
    }
    return JPP_BLE_MSG_OK;
}

/* ---- Receiver ------------------------------------------------------------ */

jpp_ble_msg_result_t jpp_ble_msg_host_recv(jpp_sdk_context_t *ctx,
                                           uint8_t *out_buf,
                                           size_t out_cap,
                                           size_t *out_len,
                                           uint32_t timeout_ms)
{
    if (ctx == NULL || out_buf == NULL || out_len == NULL) {
        return JPP_BLE_MSG_ERR_ARG;
    }

    static uint8_t rx[JPP_SDK_BLE_HOST_RX_MAX];
    bool     have_begin = false;
    uint32_t total = 0u;
    uint32_t want_crc = 0u;
    uint32_t off = 0u;
    uint16_t next_seq = 0u;
    uint32_t wait = timeout_ms;

    for (;;) {
        size_t rx_len = sizeof(rx);
        bool   received = false;
        jpp_broker_result_t result;
        if (jpp_sdk_ble_host_wait_write(ctx, rx, &rx_len, wait,
                                        &received, &result) != JPP_SDK_STATUS_OK ||
            !received) {
            return JPP_BLE_MSG_ERR_TIMEOUT;
        }
        if (rx_len < 1u) {
            continue;  /* empty write; ignore */
        }

        if (rx[0] == MSG_FRAME_BEGIN) {
            if (rx_len < MSG_BEGIN_LEN) {
                return JPP_BLE_MSG_ERR_PROTOCOL;
            }
            total    = rd_u32le(&rx[1]);
            want_crc = rd_u32le(&rx[5]);
            if (total > out_cap) {
                return JPP_BLE_MSG_ERR_TOO_BIG;
            }
            off = 0u;
            next_seq = 0u;
            have_begin = true;
            if (total == 0u) {
                *out_len = 0u;
                return (crc32_buf(out_buf, 0u) == want_crc) ? JPP_BLE_MSG_OK
                                                            : JPP_BLE_MSG_ERR_CRC;
            }
            wait = (timeout_ms < MSG_CHUNK_TIMEOUT_MS) ? timeout_ms
                                                       : MSG_CHUNK_TIMEOUT_MS;
        } else if (rx[0] == MSG_FRAME_DATA) {
            if (!have_begin) {
                continue;  /* stray data before a BEGIN; ignore */
            }
            if (rx_len < MSG_DATA_HDR) {
                return JPP_BLE_MSG_ERR_PROTOCOL;
            }
            uint16_t seq = rd_u16le(&rx[1]);
            if (seq != next_seq) {
                return JPP_BLE_MSG_ERR_PROTOCOL;
            }
            size_t n = rx_len - MSG_DATA_HDR;
            if ((size_t)off + n > total) {
                return JPP_BLE_MSG_ERR_PROTOCOL;  /* overruns declared length */
            }
            memcpy(out_buf + off, &rx[MSG_DATA_HDR], n);
            off += (uint32_t)n;
            next_seq++;
            if (off == total) {
                if (crc32_buf(out_buf, total) != want_crc) {
                    return JPP_BLE_MSG_ERR_CRC;
                }
                *out_len = total;
                return JPP_BLE_MSG_OK;
            }
        } else if (!have_begin) {
            continue;  /* unknown frame before a transfer; ignore */
        } else {
            return JPP_BLE_MSG_ERR_PROTOCOL;
        }
    }
}
