#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpp_sdk_bridge.h"
#include "jpp_crypto_core.h"
#include "meetapp_identity.h"

#define MEETAPP_PROOF_MSG_MAX 2048u

/*
 * A single participant entry for the proof, listed in all_pubkeys order.
 * The order must match the order used for MuSig2 key aggregation so that
 * any verifier can recompute the same aggregate public key.
 */
typedef struct {
    uint8_t pubkey[JPP_CRYPTO_PUBKEY_BYTES];
    char    nickname[MEETAPP_NICKNAME_MAX + 1u];
} meetapp_proof_participant_t;

/*
 * Build the human-readable proof message and compute its SHA-512 hash.
 *
 * participants[] must be in the SAME order as the all_pubkeys array used for
 * MuSig2 operations (leader at index 0, then peers in discovery order).
 * out_msg must be at least MEETAPP_PROOF_MSG_MAX bytes.
 */
void meetapp_proof_build_message(
    size_t                             n_participants,
    const meetapp_proof_participant_t *participants,
    const uint8_t                      session_nonce[8],
    const char                        *timestamp,
    char                              *out_msg,
    size_t                             out_msg_size,
    uint8_t                            out_hash[64]
);

/*
 * Write the completed proof file to the SD card.
 * Filename: proof_<8-hex-nonce>_<timestamp_compact>.txt
 *
 * The proof message embeds every participant's nickname and the timestamp, so a
 * participant MUST call this with the leader's nicknames (in all_pubkeys order)
 * and the leader's timestamp — both forwarded over round-1.5 — so the rebuilt
 * message hashes to the value that was actually signed. Rebuilding with local
 * placeholders/timestamp would produce an unverifiable proof.
 */
bool meetapp_proof_save(jpp_sdk_context_t                 *ctx,
                        size_t                             n_participants,
                        const meetapp_proof_participant_t *participants,
                        const uint8_t                      session_nonce[8],
                        const char                        *timestamp,
                        const uint8_t                      final_sig[JPP_CRYPTO_SIG_BYTES]);
