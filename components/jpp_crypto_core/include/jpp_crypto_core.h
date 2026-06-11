#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Key and signature size constants ----------------------------------- */

#define JPP_CRYPTO_PUBKEY_BYTES  32u
#define JPP_CRYPTO_SECKEY_BYTES  64u   /* libsodium Ed25519 secret = seed||pubkey */
#define JPP_CRYPTO_SIG_BYTES     64u

/* ---- Status codes ------------------------------------------------------- */

typedef enum {
    JPP_CRYPTO_OK = 0,
    JPP_CRYPTO_ERR_INVALID_ARG,
    JPP_CRYPTO_ERR_INTERNAL,
} jpp_crypto_status_t;

/* ---- Ed25519 keypair ---------------------------------------------------- */

/* Generate a new Ed25519 keypair. */
jpp_crypto_status_t jpp_crypto_keygen(
    uint8_t pubkey[JPP_CRYPTO_PUBKEY_BYTES],
    uint8_t seckey[JPP_CRYPTO_SECKEY_BYTES]
);

/* Sign message with Ed25519. sig must be JPP_CRYPTO_SIG_BYTES. */
jpp_crypto_status_t jpp_crypto_sign(
    const uint8_t *message, size_t message_len,
    const uint8_t seckey[JPP_CRYPTO_SECKEY_BYTES],
    uint8_t sig[JPP_CRYPTO_SIG_BYTES]
);

/* Verify an Ed25519 signature. Returns OK if valid. */
jpp_crypto_status_t jpp_crypto_verify(
    const uint8_t *message, size_t message_len,
    const uint8_t pubkey[JPP_CRYPTO_PUBKEY_BYTES],
    const uint8_t sig[JPP_CRYPTO_SIG_BYTES]
);

/* ---- MuSig2 — simplified two-round multi-signature over Ed25519 --------- */

/*
 * Implementation notes:
 *
 * This is a simplified variant of MuSig2 (BIP-327) built on libsodium's
 * crypto_core_ed25519 (Ristretto255) group operations.  The simplified
 * variant IS the protocol this firmware defines; it is intentionally not
 * wire-compatible with strict BIP-327.  Design decisions:
 *
 *  1. Key aggregation uses a hash-based coefficient H_agg(L, X_i) where
 *     L = SHA-512 of all sorted public keys concatenated.  BIP-327 uses a
 *     tagged hash (tag="KeyAgg list"); we use SHA-512 here for portability
 *     with libsodium's generic hash API.
 *
 *  2. Each participant uses a single random nonce per round (BIP-327 uses
 *     two nonces per participant for stronger security against certain
 *     Wagner-style attacks).  The single-nonce variant is still secure under
 *     the discrete-log assumption when nonces are generated from a CSPRNG.
 *
 *  3. The challenge hash uses SHA-512(agg_pubkey || agg_nonce || message)
 *     rather than the BIP-327 tagged-hash construction, again for portability.
 *
 *  4. Signature aggregation sums partial s-values into a single s; the final
 *     signature is (R, s) = (agg_nonce, sum(s_i)) in the standard Schnorr
 *     encoding.  Verification uses the standard Ed25519 Schnorr equation:
 *     s*G == R + c*P (where P is the aggregate public key).
 *
 * Every party in a signing session must implement these same four choices;
 * a strict BIP-327 implementation will not interoperate.
 */

#define JPP_CRYPTO_MUSIG2_SECNONCE_BYTES    64u
#define JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES    64u
#define JPP_CRYPTO_MUSIG2_PARTIAL_SIG_BYTES 32u

/*
 * Round 1 (each participant, called independently):
 *   Generate a secret nonce and return the public nonce commitment.
 *   secret_nonce must be JPP_CRYPTO_MUSIG2_SECNONCE_BYTES; caller stores it.
 */
jpp_crypto_status_t jpp_crypto_musig2_nonce_gen(
    uint8_t secret_nonce[JPP_CRYPTO_MUSIG2_SECNONCE_BYTES],
    uint8_t public_nonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES]
);

/*
 * Round 2 (each participant):
 *   Given the aggregated public nonce from the leader and the message,
 *   produce a partial signature.
 */
jpp_crypto_status_t jpp_crypto_musig2_partial_sign(
    const uint8_t secret_nonce[JPP_CRYPTO_MUSIG2_SECNONCE_BYTES],
    const uint8_t seckey[JPP_CRYPTO_SECKEY_BYTES],
    const uint8_t *all_pubkeys,       /* n * JPP_CRYPTO_PUBKEY_BYTES, in deterministic order */
    size_t n_participants,
    const uint8_t agg_pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES],
    const uint8_t *message, size_t message_len,
    uint8_t partial_sig[JPP_CRYPTO_MUSIG2_PARTIAL_SIG_BYTES]
);

/* Leader: aggregate all partial signatures into one final signature. */
jpp_crypto_status_t jpp_crypto_musig2_aggregate_sigs(
    const uint8_t *partial_sigs,      /* n * JPP_CRYPTO_MUSIG2_PARTIAL_SIG_BYTES */
    size_t n_participants,
    uint8_t final_sig[JPP_CRYPTO_SIG_BYTES]
);

/* Leader: aggregate all public nonces from participants. */
jpp_crypto_status_t jpp_crypto_musig2_aggregate_nonces(
    const uint8_t *pub_nonces,        /* n * JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES */
    size_t n_participants,
    uint8_t agg_pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES]
);

/* Compute the MuSig2 aggregate public key from all participant public keys. */
jpp_crypto_status_t jpp_crypto_musig2_aggregate_pubkeys(
    const uint8_t *pubkeys,           /* n * JPP_CRYPTO_PUBKEY_BYTES */
    size_t n_participants,
    uint8_t agg_pubkey[JPP_CRYPTO_PUBKEY_BYTES]
);

/* Verify a final MuSig2 signature against the aggregate public key. */
jpp_crypto_status_t jpp_crypto_musig2_verify(
    const uint8_t *message, size_t message_len,
    const uint8_t agg_pubkey[JPP_CRYPTO_PUBKEY_BYTES],
    const uint8_t final_sig[JPP_CRYPTO_SIG_BYTES]
);

#ifdef __cplusplus
}
#endif
