#pragma once

/*
 * Limited Run Verification (LRV) — NVS storage and cryptographic operations.
 *
 * LRV data is stored encrypted in the NVS namespace "jpp_lrv" until the user
 * unlocks it by entering the password printed on the device's inner sticker.
 * After unlock the decrypted fields are persisted to NVS alongside the cached
 * password so future backups can re-encrypt the blob automatically.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_LRV_CERT_MAX   192u   /* maximum certificate text length including NUL */
#define JPP_LRV_HWID_MAX    24u   /* "AA:BB:CC:DD:EE:FF\0" = 18 chars, padded */
#define JPP_LRV_PASS_MAX    64u   /* maximum unlock password length */

typedef enum {
    JPP_LRV_OK = 0,
    JPP_LRV_ERR_NOT_FOUND,
    JPP_LRV_ERR_LOCKED,
    JPP_LRV_ERR_WRONG_PASSWORD,
    JPP_LRV_ERR_CORRUPT,
    JPP_LRV_ERR_INTERNAL,
} jpp_lrv_result_t;

typedef struct {
    uint16_t serial;
    uint8_t  device_pubkey[32];
    uint8_t  device_seckey[64];
    uint8_t  cert_sig[64];
    char     hwid[JPP_LRV_HWID_MAX]; /* eFuse MAC, e.g. "AA:BB:CC:DD:EE:FF" */
    char     cert[JPP_LRV_CERT_MAX]; /* manufacturer-signed certificate text */
} jpp_lrv_data_t;

/* Returns true if any LRV data (encrypted or unlocked) exists in NVS. */
bool jpp_lrv_has_data(void);

/* Returns true if the LRV blob has been decrypted and individual keys are in NVS. */
bool jpp_lrv_is_unlocked(void);

/*
 * Decrypt the stored LRV blob with the given password.
 * On success the decrypted fields and the password are persisted to NVS
 * and the encrypted blob is removed.
 *
 * Returns:
 *   JPP_LRV_OK              — decryption succeeded, data is now accessible
 *   JPP_LRV_ERR_WRONG_PASSWORD — MAC verification failed (bad password)
 *   JPP_LRV_ERR_NOT_FOUND   — no encrypted blob in NVS
 *   JPP_LRV_ERR_CORRUPT     — blob too short or malformed
 *   JPP_LRV_ERR_INTERNAL    — NVS or alloc failure
 */
jpp_lrv_result_t jpp_lrv_unlock(const char *password);

/*
 * Populate display strings.  Only valid after unlock.
 * pubkey_str receives "ABCDEF-GHIJKL" (first 6 + last 6 lowercase hex chars).
 * Any of the out-parameters may be NULL.
 */
void jpp_lrv_get_display_info(uint16_t *serial, char pubkey_str[16]);

/* Copy all LRV fields into *out.  Only valid after unlock. */
jpp_lrv_result_t jpp_lrv_get_full_data(jpp_lrv_data_t *out);

/*
 * Sign challenge with the device private key.
 * challenge is a NUL-terminated string (e.g. "Alice|2026-06-08T14:02:00Z").
 * sig receives 64 bytes of Ed25519 signature.
 * Only valid after unlock.
 */
jpp_lrv_result_t jpp_lrv_sign_challenge(const char *challenge,
                                          uint8_t sig[64]);

/*
 * Return the encrypted blob suitable for embedding in a settings backup.
 * If the data is currently unlocked the plaintext is re-encrypted with the
 * cached password before returning.
 * Caller must free(*buf) when done.
 */
jpp_lrv_result_t jpp_lrv_get_encrypted_blob(uint8_t **buf, size_t *len);

/* Write an encrypted blob to NVS, replacing any existing LRV data. */
jpp_lrv_result_t jpp_lrv_store_encrypted_blob(const uint8_t *blob, size_t len);

#ifdef __cplusplus
}
#endif
