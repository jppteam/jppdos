#include "meetapp_proof.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "jpp_broker_core.h"
#include "sodium/crypto_hash_sha512.h"

static const char *TAG = "meetapp_proof";

/* ---- Message builder ----------------------------------------------------- */

void meetapp_proof_build_message(
    size_t                             n_participants,
    const meetapp_proof_participant_t *participants,
    const uint8_t                      session_nonce[8],
    const char                        *timestamp,
    char                              *out_msg,
    size_t                             out_msg_size,
    uint8_t                            out_hash[64]
)
{
    char nonce_hex[17];
    for (size_t i = 0u; i < 8u; i++) {
        snprintf(&nonce_hex[i * 2u], 3u, "%02x", session_nonce[i]);
    }
    nonce_hex[16] = '\0';

    size_t off = 0u;
    off += snprintf(out_msg + off, out_msg_size - off,
                    "J++DEVICE PROOF OF MEETUP\n"
                    "-----BEGIN MESSAGE-----\n"
                    "This verifies that %zu device%s have met up in person on %s. "
                    "Message is signed by participants' J++Devices using MuSig2 algorithm.\n"
                    "Session nonce: %s\n\n"
                    "Participants and their public keys:\n",
                    n_participants,
                    n_participants == 1u ? "" : "s",
                    timestamp,
                    nonce_hex);

    for (size_t i = 0u; i < n_participants && off < out_msg_size; i++) {
        char pk_hex[65];
        for (size_t j = 0u; j < 32u; j++) {
            snprintf(&pk_hex[j * 2u], 3u, "%02x", participants[i].pubkey[j]);
        }
        pk_hex[64] = '\0';
        off += snprintf(out_msg + off, out_msg_size - off,
                        "%-16s %s\n",
                        participants[i].nickname, pk_hex);
    }

    if (off >= out_msg_size) off = out_msg_size - 1u;
    out_msg[off] = '\0';

    crypto_hash_sha512(out_hash, (const uint8_t *)out_msg, off);
}

/* ---- Proof file writer ---------------------------------------------------- */

bool meetapp_proof_save(jpp_sdk_context_t                 *ctx,
                        size_t                             n_participants,
                        const meetapp_proof_participant_t *participants,
                        const uint8_t                      session_nonce[8],
                        const char                        *timestamp,
                        const uint8_t                      final_sig[JPP_CRYPTO_SIG_BYTES])
{
    static char msg[MEETAPP_PROOF_MSG_MAX];
    uint8_t msg_hash[64];
    meetapp_proof_build_message(n_participants, participants,
                                session_nonce, timestamp,
                                msg, sizeof(msg), msg_hash);

    char sig_hex[129];
    for (size_t i = 0u; i < 64u; i++) {
        snprintf(&sig_hex[i * 2u], 3u, "%02x", final_sig[i]);
    }
    sig_hex[128] = '\0';

    static char proof[MEETAPP_PROOF_MSG_MAX + 256u];
    size_t off = snprintf(proof, sizeof(proof), "%s", msg);
    off += snprintf(proof + off, sizeof(proof) - off,
                    "-----BEGIN SIGNATURE-----\n%s\n-----END SIGNATURE-----\n",
                    sig_hex);
    (void)off;

    char nonce_hex[17];
    for (size_t i = 0u; i < 8u; i++) {
        snprintf(&nonce_hex[i * 2u], 3u, "%02x", session_nonce[i]);
    }
    nonce_hex[16] = '\0';

    char ts_compact[14] = "00000000_0000";
    if (strlen(timestamp) >= 16u) {
        ts_compact[0]  = timestamp[0];  ts_compact[1]  = timestamp[1];
        ts_compact[2]  = timestamp[2];  ts_compact[3]  = timestamp[3];
        ts_compact[4]  = timestamp[5];  ts_compact[5]  = timestamp[6];
        ts_compact[6]  = timestamp[8];  ts_compact[7]  = timestamp[9];
        ts_compact[8]  = '_';
        ts_compact[9]  = timestamp[11]; ts_compact[10] = timestamp[12];
        ts_compact[11] = timestamp[14]; ts_compact[12] = timestamp[15];
        ts_compact[13] = '\0';
    }

    char filename[64];
    snprintf(filename, sizeof(filename), "proof_%s_%s.txt", nonce_hex, ts_compact);

    jpp_broker_result_t result;
    jpp_sdk_status_t st = jpp_sdk_file_write(ctx, filename, proof, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        ESP_LOGW(TAG, "Failed to save proof: st=%d code=%s",
                 st, result.code ? result.code : "?");
        return false;
    }

    ESP_LOGI(TAG, "Proof saved: %s", filename);
    return true;
}
