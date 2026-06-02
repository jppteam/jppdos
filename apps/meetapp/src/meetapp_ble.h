#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "jpp_sdk_bridge.h"
#include "jpp_crypto_core.h"
#include "meetapp_identity.h"

#define MEETAPP_MAX_PEERS 19u        /* max participants excluding leader */
#define MEETAPP_MAX_TOTAL 20u        /* leader + peers                   */

/* BLE advertisement company ID (big-endian "JP"). */
#define MEETAPP_COMPANY_ID_LO 0x50u
#define MEETAPP_COMPANY_ID_HI 0x4Au

/* AD type bytes embedded in manufacturer-specific payloads. */
#define MEETAPP_AD_PING 0x01u
#define MEETAPP_AD_PONG 0x02u

/* MeetApp uses the App SDK's generic GATT app-host service for its peer
   exchange; the service/characteristic UUIDs come from the SDK
   (JPP_SDK_BLE_HOST_SVC_UUID / _TX_UUID / _RX_UUID). */

/* RX characteristic data type tags. */
#define MEETAPP_RX_ROUND15  0x01u  /* round-1.5: msg_hash + agg_nonce + n + pubkeys */
#define MEETAPP_RX_FINALSIG 0x02u  /* final signature */

/* TX characteristic data type tags. */
#define MEETAPP_TX_NONCE    0x01u  /* pubkey + public_nonce */
#define MEETAPP_TX_PARTSIG  0x02u  /* partial signature */

typedef struct {
    char    nickname[MEETAPP_NICKNAME_MAX + 1u];
    uint8_t pubkey[JPP_CRYPTO_PUBKEY_BYTES];
    uint8_t pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES];
    uint8_t partial_sig[JPP_CRYPTO_MUSIG2_PARTIAL_SIG_BYTES];
    char    ble_addr[JPP_SDK_BLE_ADDR_LEN];
} meetapp_peer_t;

typedef struct {
    uint8_t         session_nonce[8];
    meetapp_peer_t  peers[MEETAPP_MAX_PEERS];
    size_t          peer_count;
} meetapp_session_t;

/* Build and broadcast a PING or PONG advertisement payload. */
void meetapp_ble_build_ad(uint8_t type,
                           const uint8_t session_nonce[8],
                           const char *nickname,
                           uint8_t *out_ad, size_t *out_len);

/*
 * Scan for PONG packets matching session_nonce and populate session->peers.
 * Blocks for scan_ms milliseconds.  Returns the number of new peers found.
 */
size_t meetapp_ble_scan_pongs(jpp_sdk_context_t *ctx,
                               meetapp_session_t *session,
                               uint32_t scan_ms);

/*
 * Leader round A: connect to each peer and read TX char (pubkey + nonce).
 * Returns false on fatal error.
 */
bool meetapp_ble_collect_nonces(jpp_sdk_context_t *ctx,
                                 const meetapp_identity_t *identity,
                                 meetapp_session_t *session);

/*
 * Leader round B: connect to each peer, write round-1.5 data, wait, read
 * partial sig.  msg_hash is SHA-512(message_text).  Returns false on error.
 */
bool meetapp_ble_exchange_sigs(jpp_sdk_context_t *ctx,
                                meetapp_session_t *session,
                                const uint8_t msg_hash[64],
                                const uint8_t agg_pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES],
                                const uint8_t *all_pubkeys, /* (1+n)*32 leader-first */
                                size_t n_total);

/*
 * Leader round C: connect to each peer and write the final signature.
 */
bool meetapp_ble_distribute_sig(jpp_sdk_context_t *ctx,
                                 meetapp_session_t *session,
                                 const uint8_t final_sig[JPP_CRYPTO_SIG_BYTES]);

/*
 * Participant: perform a single scan slice of scan_ms looking for a leader's
 * PING. Returns true and fills session_nonce / leader_addr if one was seen.
 * Designed to be called repeatedly so the caller can animate and poll for a
 * cancel between slices.
 */
bool meetapp_ble_scan_ping_once(jpp_sdk_context_t *ctx,
                                 uint8_t session_nonce[8],
                                 char leader_addr[JPP_SDK_BLE_ADDR_LEN],
                                 uint32_t scan_ms);

/*
 * Participant: wait for round-1.5 data from the leader (via the host RX
 * characteristic write). Returns true on success; fills msg_hash, agg_pubnonce,
 * all_pubkeys, n_total. all_pubkeys must be at least MEETAPP_MAX_TOTAL*32 bytes.
 */
bool meetapp_ble_wait_round15(jpp_sdk_context_t *ctx,
                               uint8_t msg_hash[64],
                               uint8_t agg_pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES],
                               uint8_t *all_pubkeys,
                               size_t *n_total,
                               uint32_t timeout_ms);

/*
 * Participant: wait for the final signature (via the host RX characteristic
 * write). Returns true on success; fills final_sig.
 */
bool meetapp_ble_wait_final_sig(jpp_sdk_context_t *ctx,
                                 uint8_t final_sig[JPP_CRYPTO_SIG_BYTES],
                                 uint32_t timeout_ms);
