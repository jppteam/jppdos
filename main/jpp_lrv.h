#pragma once

/*
 * Limited Run Verification (LRV) — AT24C32 EEPROM storage and cryptographic
 * operations.
 *
 * The LRV identity lives on the external AT24C32 EEPROM (I2C 0x50) that rides on
 * the DS1307 RTC breakout board, NOT in NVS.  This makes the identity survive a
 * factory reset and a full firmware reflash, binding it permanently to the RTC
 * module.  The EEPROM holds two regions:
 *
 *   - IDENTITY (write-once): the password-encrypted blob.  It is written exactly
 *     once during manufacturing (see CONFIG_JPP_LRV_PROVISIONING) and is never
 *     mutated or erased by production firmware.
 *   - STATE (writable): caches the sticker password after the user unlocks, so
 *     the device stays unlocked across reboots.
 *
 * After unlock the decrypted fields exist only in RAM (never persisted); on each
 * boot they are re-derived from the immutable blob using the cached password.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "jpp_rtc_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_LRV_CHALLENGE_MAX 128u  /* "{username}|{iso8601}" incl. NUL */

#define JPP_LRV_CERT_MAX   192u   /* maximum certificate text length including NUL */
#define JPP_LRV_HWID_MAX    24u   /* "AA:BB:CC:DD:EE:FF\0" = 18 chars, padded */
#define JPP_LRV_PASS_MAX    64u   /* maximum unlock password length */

typedef enum {
    JPP_LRV_OK = 0,
    JPP_LRV_ERR_NOT_FOUND,
    JPP_LRV_ERR_LOCKED,
    JPP_LRV_ERR_WRONG_PASSWORD,
    JPP_LRV_ERR_CORRUPT,
    JPP_LRV_ERR_EXISTS,       /* provisioning: identity already written (write-once) */
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

/*
 * Bind the LRV subsystem to the shared I2C bus and load state from the AT24C32
 * EEPROM.  Probes the chip (0x50); if an identity blob is present and a cached
 * password is stored, the blob is decrypted into RAM so the device comes up
 * already unlocked.  Must be called once at boot (after I2C init) before any
 * other jpp_lrv_* call.  bus may be NULL (no EEPROM ⇒ no LRV data).
 */
void jpp_lrv_init(i2c_master_bus_handle_t bus);

/* Returns true if an LRV identity blob is present on the EEPROM. */
bool jpp_lrv_has_data(void);

/* Returns true if the blob has been decrypted (keys available in RAM). */
bool jpp_lrv_is_unlocked(void);

/*
 * Re-lock: clear the cached password (EEPROM STATE region) and wipe the
 * decrypted RAM copy.  The immutable identity blob is left intact, so the device
 * keeps its identity but requires the sticker password again.  Called on factory
 * reset.
 */
void jpp_lrv_relock(void);

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
 * Parse "run_size=N" out of the certificate text (run_size has no separate
 * field — see the struct comment on jpp_lrv_data_t.cert).  Only valid after
 * unlock.  Returns JPP_LRV_ERR_CORRUPT if the certificate has no such line.
 */
jpp_lrv_result_t jpp_lrv_get_run_size(uint16_t *out_run_size);

/*
 * Build the canonical LRV challenge string into out:
 *   "{username}|{YYYY-MM-DDTHH:MM:SSZ}"
 * username comes from NVS jpp_user/username (empty when unset).  The timestamp
 * is read from `rtc`; if rtc is NULL or unreadable the epoch placeholder
 * "1970-01-01T00:00:00Z" is used.  Every challenge producer (settings verify,
 * serial manager, HTTP verification server) must use this builder so the
 * signed bytes match what the verification service reconstructs.
 *
 * Optional out-parameters (any may be NULL): out_username receives the raw
 * username, out_dt/out_has_time the datetime used for the timestamp.
 */
void jpp_lrv_build_challenge(jpp_rtc_state_t *rtc,
                              char *out, size_t out_len,
                              char *out_username, size_t username_len,
                              jpp_rtc_datetime_t *out_dt, bool *out_has_time);

/*
 * Sign challenge with the device private key.
 * challenge is a NUL-terminated string (e.g. "Alice|2026-06-08T14:02:00Z").
 * sig receives 64 bytes of Ed25519 signature.
 * Only valid after unlock.
 */
jpp_lrv_result_t jpp_lrv_sign_challenge(const char *challenge,
                                          uint8_t sig[64]);

/*
 * Return a heap copy of the immutable encrypted identity blob from the EEPROM.
 * Caller must free(*buf) when done.  Returns JPP_LRV_ERR_NOT_FOUND when no
 * identity is provisioned.
 */
jpp_lrv_result_t jpp_lrv_get_encrypted_blob(uint8_t **buf, size_t *len);

/*
 * Provisioning-only (compiled in only when CONFIG_JPP_LRV_PROVISIONING=y):
 * write the encrypted blob to the write-once EEPROM IDENTITY region.  Refuses
 * with JPP_LRV_ERR_EXISTS if an identity is already provisioned.  Absent from
 * production firmware entirely.
 */
jpp_lrv_result_t jpp_lrv_store_encrypted_blob(const uint8_t *blob, size_t len);

/*
 * Format data as uppercase hex into buf (always NUL-terminated).
 * bytes_per_row > 0 groups output into space-separated rows of that many
 * bytes (newline between rows); 0 yields one contiguous string.
 */
void jpp_lrv_hex_format(const uint8_t *data, size_t len,
                        size_t bytes_per_row, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
