#pragma once

/*
 * jpp_ble_msg — arbitrary-length message transfer over the App SDK's BLE GATT
 * primitives.
 *
 * A single GATT characteristic value is capped at 512 bytes (BLE_ATT_ATTR_MAX_LEN,
 * a Bluetooth spec limit — not tunable). To move a larger payload, the sender
 * (GATT central) splits it into framed chunks and writes them one at a time to
 * the peer's RX characteristic; the receiver (GATT host/peripheral) reassembles
 * them from successive `ble_host_wait_write` deliveries.
 *
 * Wire framing (each frame is one characteristic write, binary on-wire):
 *   BEGIN: [0x01][total_len: u32 LE][crc32: u32 LE]
 *   DATA:  [0x02][seq: u16 LE][payload ...]        (payload <= JPP_BLE_MSG_CHUNK_MAX)
 * The receiver completes when it has `total_len` bytes and the CRC-32/ISO-HDLC
 * (zlib) of the reassembly matches. DATA frames must arrive in order (seq 0,1,…).
 *
 * Flow control relies on the SDK's writes being acknowledged (ATT Write
 * Request / Prepare-Execute): the central cannot send frame N+1 until the
 * peripheral has ACKed frame N, which happens only after its RX callback has
 * copied the frame out — so the single host RX buffer is never overwritten
 * before the receiver drains it. One transfer at a time per connection.
 *
 * This is an app-side helper layered over jpp_sdk_ble_* — it is compiled into
 * each app that uses it, not part of the firmware SDK surface.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpp_sdk_bridge.h"

/* Max data bytes per DATA frame. Frame size = 3 + this, kept well under the
   512-byte ATT value limit so every chunk is a single characteristic write. */
#define JPP_BLE_MSG_CHUNK_MAX 400u

typedef enum {
    JPP_BLE_MSG_OK = 0,
    JPP_BLE_MSG_ERR_ARG,       /* bad argument */
    JPP_BLE_MSG_ERR_WRITE,     /* a characteristic write failed */
    JPP_BLE_MSG_ERR_TIMEOUT,   /* receiver: no frame arrived before the timeout */
    JPP_BLE_MSG_ERR_PROTOCOL,  /* malformed or out-of-order frame */
    JPP_BLE_MSG_ERR_TOO_BIG,   /* message exceeds the receive buffer */
    JPP_BLE_MSG_ERR_CRC,       /* reassembled payload failed the CRC check */
} jpp_ble_msg_result_t;

/*
 * Sender (GATT central). Chunk `data` (`len` bytes) and write it to
 * `char_uuid` on the connected peer `conn`. `len` may be 0 (sends an empty
 * message). Returns JPP_BLE_MSG_OK on success, JPP_BLE_MSG_ERR_WRITE if any
 * chunk write is rejected.
 */
jpp_ble_msg_result_t jpp_ble_msg_send(jpp_sdk_context_t *ctx,
                                      jpp_sdk_ble_conn_t conn,
                                      const char *svc_uuid,
                                      const char *char_uuid,
                                      const uint8_t *data,
                                      size_t len);

/*
 * Receiver (GATT host/peripheral). Reassemble a message written to the host RX
 * characteristic into `out_buf` (capacity `out_cap`); the byte count lands in
 * `*out_len`. `timeout_ms` bounds the wait for the first (BEGIN) frame; once a
 * transfer is in progress a shorter internal per-chunk timeout applies so a
 * stalled sender can't hang the caller for the full `timeout_ms`. Requires
 * ble.host. Call ble_host_clear first if a stale write may be buffered.
 */
jpp_ble_msg_result_t jpp_ble_msg_host_recv(jpp_sdk_context_t *ctx,
                                           uint8_t *out_buf,
                                           size_t out_cap,
                                           size_t *out_len,
                                           uint32_t timeout_ms);
