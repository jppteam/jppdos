/*
 * jpp_crypto_core.c — Ed25519 signing and simplified MuSig2 multi-signature.
 *
 * All cryptographic operations use libsodium.
 *
 * MuSig2 notes (see header for full deviation list):
 *   - Key aggregation: H_agg(L, X_i) via SHA-512; single-nonce per participant.
 *   - Challenge: SHA-512(agg_pubkey || agg_nonce || message).
 *   - Final sig format: first 32 bytes = agg_nonce (R), last 32 bytes = sum(s_i).
 *
 * TODO (for strict BIP-327 compliance):
 *   - Replace SHA-512 with BIP-327 tagged hashes.
 *   - Use two nonces per participant (r1, r2) and the b-factor blinding.
 *   - Sort public keys lexicographically before computing L.
 */

#include "../include/jpp_crypto_core.h"

#include <string.h>
#include <stdlib.h>

#include "sodium.h"
#include "sodium/crypto_sign_ed25519.h"
#include "sodium/crypto_hash_sha512.h"
#include "sodium/crypto_core_ed25519.h"
#include "sodium/crypto_scalarmult_ed25519.h"
#include "sodium/randombytes.h"

/* ---- Helpers ------------------------------------------------------------ */

/*
 * Reduce a 64-byte hash modulo the Ed25519 group order into a 32-byte scalar.
 * Uses crypto_core_ed25519_scalar_reduce (libsodium >= 1.0.18).
 */
static void hash64_to_scalar(const uint8_t hash[64], uint8_t scalar[32])
{
    crypto_core_ed25519_scalar_reduce(scalar, hash);
}

/* ---- Ed25519 ------------------------------------------------------------ */

jpp_crypto_status_t jpp_crypto_keygen(
    uint8_t pubkey[JPP_CRYPTO_PUBKEY_BYTES],
    uint8_t seckey[JPP_CRYPTO_SECKEY_BYTES]
)
{
    if (pubkey == NULL || seckey == NULL) {
        return JPP_CRYPTO_ERR_INVALID_ARG;
    }
    if (crypto_sign_ed25519_keypair(pubkey, seckey) != 0) {
        return JPP_CRYPTO_ERR_INTERNAL;
    }
    return JPP_CRYPTO_OK;
}

jpp_crypto_status_t jpp_crypto_sign(
    const uint8_t *message, size_t message_len,
    const uint8_t seckey[JPP_CRYPTO_SECKEY_BYTES],
    uint8_t sig[JPP_CRYPTO_SIG_BYTES]
)
{
    if (message == NULL || seckey == NULL || sig == NULL) {
        return JPP_CRYPTO_ERR_INVALID_ARG;
    }
    unsigned long long sig_len = 0u;
    if (crypto_sign_ed25519_detached(sig, &sig_len, message, message_len, seckey) != 0) {
        return JPP_CRYPTO_ERR_INTERNAL;
    }
    return JPP_CRYPTO_OK;
}

jpp_crypto_status_t jpp_crypto_verify(
    const uint8_t *message, size_t message_len,
    const uint8_t pubkey[JPP_CRYPTO_PUBKEY_BYTES],
    const uint8_t sig[JPP_CRYPTO_SIG_BYTES]
)
{
    if (message == NULL || pubkey == NULL || sig == NULL) {
        return JPP_CRYPTO_ERR_INVALID_ARG;
    }
    if (crypto_sign_ed25519_verify_detached(sig, message, message_len, pubkey) != 0) {
        return JPP_CRYPTO_ERR_INTERNAL;
    }
    return JPP_CRYPTO_OK;
}

/* ---- MuSig2 — key aggregation ------------------------------------------ */

/*
 * Compute L = SHA-512(X_0 || X_1 || ... || X_{n-1}) where X_i are the
 * raw 32-byte public keys in the order supplied by the caller.
 *
 * BIP-327 deviation: BIP-327 sorts keys lexicographically before hashing.
 * TODO: Sort all_pubkeys before computing L for strict compliance.
 */
static void compute_key_list_hash(
    const uint8_t *pubkeys,
    size_t n,
    uint8_t out_hash[64]
)
{
    crypto_hash_sha512_state st;
    crypto_hash_sha512_init(&st);
    for (size_t i = 0u; i < n; i++) {
        crypto_hash_sha512_update(&st, pubkeys + i * JPP_CRYPTO_PUBKEY_BYTES,
                                  JPP_CRYPTO_PUBKEY_BYTES);
    }
    crypto_hash_sha512_final(&st, out_hash);
}

/*
 * Compute the key-aggregation coefficient a_i = H_agg(L, X_i) as a scalar.
 *
 * BIP-327 deviation: uses SHA-512(L || X_i) rather than a tagged hash.
 */
static void compute_key_coeff(
    const uint8_t L[64],
    const uint8_t pubkey[JPP_CRYPTO_PUBKEY_BYTES],
    uint8_t coeff[32]
)
{
    uint8_t hash[64];
    crypto_hash_sha512_state st;
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, L, 64u);
    crypto_hash_sha512_update(&st, pubkey, JPP_CRYPTO_PUBKEY_BYTES);
    crypto_hash_sha512_final(&st, hash);
    hash64_to_scalar(hash, coeff);
}

jpp_crypto_status_t jpp_crypto_musig2_aggregate_pubkeys(
    const uint8_t *pubkeys,
    size_t n_participants,
    uint8_t agg_pubkey[JPP_CRYPTO_PUBKEY_BYTES]
)
{
    if (pubkeys == NULL || n_participants == 0u || agg_pubkey == NULL) {
        return JPP_CRYPTO_ERR_INVALID_ARG;
    }

    uint8_t L[64];
    compute_key_list_hash(pubkeys, n_participants, L);

    /*
     * P_agg = sum_i( a_i * X_i )
     * where a_i = H_agg(L, X_i).
     */
    uint8_t acc[32];
    bool first = true;

    for (size_t i = 0u; i < n_participants; i++) {
        uint8_t coeff[32];
        compute_key_coeff(L, pubkeys + i * JPP_CRYPTO_PUBKEY_BYTES, coeff);

        uint8_t term[32];
        /* term = coeff * X_i (scalar multiplication on Ed25519) */
        if (crypto_scalarmult_ed25519_noclamp(
                term, coeff, pubkeys + i * JPP_CRYPTO_PUBKEY_BYTES) != 0) {
            return JPP_CRYPTO_ERR_INTERNAL;
        }

        if (first) {
            memcpy(acc, term, 32u);
            first = false;
        } else {
            if (crypto_core_ed25519_add(acc, acc, term) != 0) {
                return JPP_CRYPTO_ERR_INTERNAL;
            }
        }
    }

    memcpy(agg_pubkey, acc, JPP_CRYPTO_PUBKEY_BYTES);
    return JPP_CRYPTO_OK;
}

/* ---- MuSig2 — nonce generation ----------------------------------------- */

/*
 * secret_nonce layout (64 bytes):
 *   [0..31]  = r  (random scalar, little-endian)
 *   [32..63] = R  (r * G, the corresponding curve point)
 *
 * public_nonce layout (64 bytes):
 *   [0..31]  = R  (curve point, same as secret_nonce[32..63])
 *   [32..63] = 0  (reserved; in two-nonce BIP-327 this would be R2)
 *
 * BIP-327 deviation: uses only one nonce scalar per participant.
 * TODO: Add a second nonce (r2, R2) for full BIP-327 two-nonce blinding.
 */
jpp_crypto_status_t jpp_crypto_musig2_nonce_gen(
    uint8_t secret_nonce[JPP_CRYPTO_MUSIG2_SECNONCE_BYTES],
    uint8_t public_nonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES]
)
{
    if (secret_nonce == NULL || public_nonce == NULL) {
        return JPP_CRYPTO_ERR_INVALID_ARG;
    }

    /* Generate random scalar r. */
    uint8_t r[32];
    randombytes_buf(r, sizeof(r));
    /* Clamp to a valid Ed25519 scalar. */
    crypto_core_ed25519_scalar_reduce(r, (const unsigned char[64]){
        r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
        r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15],
        r[16], r[17], r[18], r[19], r[20], r[21], r[22], r[23],
        r[24], r[25], r[26], r[27], r[28], r[29], r[30], r[31],
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    });

    /* R = r * G */
    uint8_t R[32];
    if (crypto_scalarmult_ed25519_base_noclamp(R, r) != 0) {
        return JPP_CRYPTO_ERR_INTERNAL;
    }

    memcpy(secret_nonce,        r, 32u);
    memcpy(secret_nonce + 32u,  R, 32u);

    memcpy(public_nonce,       R, 32u);
    memset(public_nonce + 32u, 0, 32u);  /* second nonce slot reserved */

    return JPP_CRYPTO_OK;
}

/* ---- MuSig2 — nonce aggregation ---------------------------------------- */

/*
 * Aggregate public nonces: R_agg = sum_i( R_i )
 *
 * BIP-327 deviation: only aggregates the first nonce slot (bytes 0..31).
 * TODO: Aggregate the second nonce slot and compute the b-factor blinding.
 */
jpp_crypto_status_t jpp_crypto_musig2_aggregate_nonces(
    const uint8_t *pub_nonces,
    size_t n_participants,
    uint8_t agg_pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES]
)
{
    if (pub_nonces == NULL || n_participants == 0u || agg_pubnonce == NULL) {
        return JPP_CRYPTO_ERR_INVALID_ARG;
    }

    uint8_t acc[32];
    memcpy(acc, pub_nonces, 32u);  /* Start with R_0 */

    for (size_t i = 1u; i < n_participants; i++) {
        if (crypto_core_ed25519_add(acc, acc,
                                    pub_nonces + i * JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES) != 0) {
            return JPP_CRYPTO_ERR_INTERNAL;
        }
    }

    memcpy(agg_pubnonce,       acc, 32u);
    memset(agg_pubnonce + 32u, 0,  32u);  /* second slot reserved */

    return JPP_CRYPTO_OK;
}

/* ---- MuSig2 — partial signing ------------------------------------------ */

/*
 * Partial signature:
 *   s_i = r_i + c * a_i * x_i  (mod group order)
 *
 * where:
 *   r_i   = secret nonce scalar (secret_nonce[0..31])
 *   c     = H(P_agg || R_agg || message)  — the Schnorr challenge
 *   a_i   = H_agg(L, X_i)                — key-aggregation coefficient
 *   x_i   = secret key scalar            — extracted from Ed25519 seckey
 *
 * The Ed25519 secret key from libsodium is (seed || pubkey).
 * The actual signing scalar x is derived from seed via SHA-512 and clamped.
 */
jpp_crypto_status_t jpp_crypto_musig2_partial_sign(
    const uint8_t secret_nonce[JPP_CRYPTO_MUSIG2_SECNONCE_BYTES],
    const uint8_t seckey[JPP_CRYPTO_SECKEY_BYTES],
    const uint8_t *all_pubkeys,
    size_t n_participants,
    const uint8_t agg_pubnonce[JPP_CRYPTO_MUSIG2_PUBNONCE_BYTES],
    const uint8_t *message, size_t message_len,
    uint8_t partial_sig[JPP_CRYPTO_MUSIG2_PARTIAL_SIG_BYTES]
)
{
    if (secret_nonce == NULL || seckey == NULL || all_pubkeys == NULL ||
        n_participants == 0u || agg_pubnonce == NULL ||
        (message == NULL && message_len > 0u) || partial_sig == NULL) {
        return JPP_CRYPTO_ERR_INVALID_ARG;
    }

    const uint8_t *r    = secret_nonce;          /* 32-byte nonce scalar */
    const uint8_t *R_agg = agg_pubnonce;          /* 32-byte aggregate nonce point */

    /* --- Extract signing scalar x from the Ed25519 seed --- */
    /* libsodium seckey = seed (32 bytes) || pubkey (32 bytes).
     * The signing scalar is SHA-512(seed), clamped per RFC 8032. */
    uint8_t seed_hash[64];
    crypto_hash_sha512(seed_hash, seckey, 32u);
    /* Clamp (RFC 8032 §5.1.5): clear bits 0,1,2 of byte 0; clear bit 7 of byte 31; set bit 6 of byte 31. */
    seed_hash[0]  &= 248u;
    seed_hash[31] &= 127u;
    seed_hash[31] |= 64u;
    /* Reduce to a scalar (take only the lower 32 bytes after clamping — this is the Ed25519 scalar). */
    uint8_t x[32];
    memcpy(x, seed_hash, 32u);

    /* --- Compute aggregate public key --- */
    uint8_t P_agg[32];
    jpp_crypto_status_t st = jpp_crypto_musig2_aggregate_pubkeys(all_pubkeys, n_participants, P_agg);
    if (st != JPP_CRYPTO_OK) {
        return st;
    }

    /* --- Compute Schnorr challenge: c = H(P_agg || R_agg || message) --- */
    uint8_t c_hash[64];
    crypto_hash_sha512_state sha_st;
    crypto_hash_sha512_init(&sha_st);
    crypto_hash_sha512_update(&sha_st, P_agg,  32u);
    crypto_hash_sha512_update(&sha_st, R_agg,  32u);
    if (message_len > 0u) {
        crypto_hash_sha512_update(&sha_st, message, message_len);
    }
    crypto_hash_sha512_final(&sha_st, c_hash);
    uint8_t c[32];
    hash64_to_scalar(c_hash, c);

    /* --- Compute key-aggregation coefficient a_i for this participant --- */
    /* Extract our own public key from seckey (bytes 32..63). */
    const uint8_t *my_pubkey = seckey + 32u;

    uint8_t L[64];
    compute_key_list_hash(all_pubkeys, n_participants, L);

    uint8_t a_i[32];
    compute_key_coeff(L, my_pubkey, a_i);

    /* --- s_i = r_i + c * a_i * x_i --- */
    /* temp1 = c * a_i */
    uint8_t temp1[32];
    crypto_core_ed25519_scalar_mul(temp1, c, a_i);

    /* temp2 = temp1 * x_i */
    uint8_t temp2[32];
    crypto_core_ed25519_scalar_mul(temp2, temp1, x);

    /* s_i = r_i + temp2 */
    uint8_t s_i[32];
    crypto_core_ed25519_scalar_add(s_i, r, temp2);

    memcpy(partial_sig, s_i, JPP_CRYPTO_MUSIG2_PARTIAL_SIG_BYTES);

    /* Clear sensitive material. */
    sodium_memzero(seed_hash, sizeof(seed_hash));
    sodium_memzero(x, sizeof(x));
    sodium_memzero(L, sizeof(L));
    sodium_memzero(a_i, sizeof(a_i));
    sodium_memzero(temp1, sizeof(temp1));
    sodium_memzero(temp2, sizeof(temp2));
    sodium_memzero(s_i, sizeof(s_i));

    return JPP_CRYPTO_OK;
}

/* ---- MuSig2 — signature aggregation ------------------------------------ */

/*
 * final_sig layout (64 bytes, matching Ed25519 wire format):
 *   [0..31]  = R_agg  (aggregate nonce point — must be stored by caller separately)
 *   [32..63] = s      = sum_i( s_i )
 *
 * NOTE: The caller must pass in the aggregate nonce as part of the final sig.
 * This function only sums partial s values.  The aggregate nonce R must be
 * prepended by the leader.
 *
 * TODO: The API should accept agg_pubnonce so this function can write R_agg
 * into final_sig[0..31] itself.  Currently the caller is responsible.
 */
jpp_crypto_status_t jpp_crypto_musig2_aggregate_sigs(
    const uint8_t *partial_sigs,
    size_t n_participants,
    uint8_t final_sig[JPP_CRYPTO_SIG_BYTES]
)
{
    if (partial_sigs == NULL || n_participants == 0u || final_sig == NULL) {
        return JPP_CRYPTO_ERR_INVALID_ARG;
    }

    /* s = sum_i( s_i ) mod group order */
    uint8_t s[32];
    memcpy(s, partial_sigs, 32u);  /* Start with s_0 */

    for (size_t i = 1u; i < n_participants; i++) {
        crypto_core_ed25519_scalar_add(s, s,
                                       partial_sigs + i * JPP_CRYPTO_MUSIG2_PARTIAL_SIG_BYTES);
    }

    /* Write s into the second half of final_sig.
     * The first 32 bytes (R_agg) must be filled by the caller. */
    memcpy(final_sig + 32u, s, 32u);

    return JPP_CRYPTO_OK;
}

/* ---- MuSig2 — verification --------------------------------------------- */

/*
 * Verify: check s*G == R + c*P_agg
 * where R = final_sig[0..31], s = final_sig[32..63],
 * and c = H(P_agg || R || message) using the same challenge construction
 * as partial_sign.
 *
 * BIP-327 deviation: final_sig must carry R_agg in its first 32 bytes
 * (written by the leader after calling jpp_crypto_musig2_aggregate_sigs).
 */
jpp_crypto_status_t jpp_crypto_musig2_verify(
    const uint8_t *message, size_t message_len,
    const uint8_t agg_pubkey[JPP_CRYPTO_PUBKEY_BYTES],
    const uint8_t final_sig[JPP_CRYPTO_SIG_BYTES]
)
{
    if (agg_pubkey == NULL || final_sig == NULL ||
        (message == NULL && message_len > 0u)) {
        return JPP_CRYPTO_ERR_INVALID_ARG;
    }

    const uint8_t *R = final_sig;
    const uint8_t *s = final_sig + 32u;

    /* c = H(P_agg || R || message) — must match partial_sign's challenge. */
    uint8_t c_hash[64];
    crypto_hash_sha512_state sha_st;
    crypto_hash_sha512_init(&sha_st);
    crypto_hash_sha512_update(&sha_st, agg_pubkey, 32u);
    crypto_hash_sha512_update(&sha_st, R, 32u);
    if (message_len > 0u) {
        crypto_hash_sha512_update(&sha_st, message, message_len);
    }
    crypto_hash_sha512_final(&sha_st, c_hash);
    uint8_t c[32];
    hash64_to_scalar(c_hash, c);

    /* sG = s * G */
    uint8_t sG[32];
    if (crypto_scalarmult_ed25519_base_noclamp(sG, s) != 0) {
        return JPP_CRYPTO_ERR_INTERNAL;
    }

    /* cP = c * P_agg */
    uint8_t cP[32];
    if (crypto_scalarmult_ed25519_noclamp(cP, c, agg_pubkey) != 0) {
        return JPP_CRYPTO_ERR_INTERNAL;
    }

    /* expected = R + cP */
    uint8_t expected[32];
    if (crypto_core_ed25519_add(expected, R, cP) != 0) {
        return JPP_CRYPTO_ERR_INTERNAL;
    }

    /* Constant-time comparison: sG == R + cP */
    if (sodium_memcmp(sG, expected, 32u) != 0) {
        return JPP_CRYPTO_ERR_INTERNAL;
    }

    return JPP_CRYPTO_OK;
}
