#pragma once

/*
 * Limited Run Verification (LRV) — AT24C32 EEPROM storage and cryptographic
 * operations.
 *
 * The LRV identity lives on the external AT24C32 EEPROM (I2C 0x50) that rides on
 * the DS1307 RTC breakout board, NOT in NVS.  This makes the identity survive a
 * factory reset and a full firmware reflash, binding it permanently to the RTC
 * module.  The EEPROM holds a single write-once IDENTITY region storing the
 * record in the clear: it is written exactly once during manufacturing (see
 * CONFIG_JPP_LRV_PROVISIONING) and is never mutated or erased by production
 * firmware.  There is no password and no encryption — the record is readable as
 * soon as the chip is on the bus, so the device comes up with its identity
 * available and every LRV consumer only has to check jpp_lrv_has_data().
 *
 * At jpp_lrv_init() the record is copied into RAM once; every accessor serves
 * that copy rather than re-reading the bus.
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

typedef enum {
    JPP_LRV_OK = 0,
    JPP_LRV_ERR_NOT_FOUND,
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
 * Bind the LRV subsystem to the shared I2C bus and load the identity from the
 * AT24C32 EEPROM.  Probes the chip (0x50); if a valid identity record is present
 * it is read into RAM so every subsequent accessor is served from memory.  Must
 * be called once at boot (after I2C init) before any other jpp_lrv_* call.
 * bus may be NULL (no EEPROM ⇒ no LRV data).
 */
void jpp_lrv_init(i2c_master_bus_handle_t bus);

/*
 * Returns true if a valid LRV identity was loaded from the EEPROM.  When this is
 * true every accessor below returns data; when it is false they all return
 * JPP_LRV_ERR_NOT_FOUND.
 */
bool jpp_lrv_has_data(void);

/*
 * Populate display strings.  Only valid when jpp_lrv_has_data().
 * pubkey_str receives "ABCDEF-GHIJKL" (first 6 + last 6 lowercase hex chars).
 * Any of the out-parameters may be NULL.
 */
void jpp_lrv_get_display_info(uint16_t *serial, char pubkey_str[16]);

/* Copy all LRV fields into *out.  Only valid when jpp_lrv_has_data(). */
jpp_lrv_result_t jpp_lrv_get_full_data(jpp_lrv_data_t *out);

/*
 * Parse "run_size=N" out of the certificate text (run_size has no separate
 * field — see the struct comment on jpp_lrv_data_t.cert).  Returns
 * JPP_LRV_ERR_CORRUPT if the certificate has no such line.
 */
jpp_lrv_result_t jpp_lrv_get_run_size(uint16_t *out_run_size);

/*
 * Build the canonical LRV challenge string into out:
 *   "{username}|{YYYY-MM-DDTHH:MM:SSZ}"
 * username comes from NVS jpp_user/username (empty when unset).  The timestamp
 * is read from `rtc`; if rtc is NULL or unreadable the epoch placeholder
 * "1970-01-01T00:00:00Z" is used.  The RTC holds local wall-clock time, so the
 * builder converts it to UTC (subtracting the NVS jpp_time/tz_h offset) before
 * emitting the "Z"-suffixed timestamp.  Every challenge producer (settings
 * verify, serial manager, HTTP verification server) must use this builder so the
 * signed bytes match what the verification service reconstructs.
 *
 * Optional out-parameters (any may be NULL): out_username receives the raw
 * username, out_dt/out_has_time the local datetime read from the RTC (not the
 * UTC-adjusted value that goes into the challenge).
 */
void jpp_lrv_build_challenge(jpp_rtc_state_t *rtc,
                              char *out, size_t out_len,
                              char *out_username, size_t username_len,
                              jpp_rtc_datetime_t *out_dt, bool *out_has_time);

/*
 * Sign challenge with the device private key.
 * challenge is a NUL-terminated string (e.g. "Alice|2026-06-08T14:02:00Z").
 * sig receives 64 bytes of Ed25519 signature.
 * Only valid when jpp_lrv_has_data().
 */
jpp_lrv_result_t jpp_lrv_sign_challenge(const char *challenge,
                                          uint8_t sig[64]);

/*
 * Provisioning-only (compiled in only when CONFIG_JPP_LRV_PROVISIONING=y):
 * write the raw identity record to the write-once EEPROM IDENTITY region.
 * `record` is the packed plaintext layout documented in jpp_lrv.c (and mirrored
 * by scripts/lrv_manufacturing.py serialise_record()).  Refuses with
 * JPP_LRV_ERR_EXISTS if an identity is already provisioned.  Absent from
 * production firmware entirely.
 */
jpp_lrv_result_t jpp_lrv_store_identity(const uint8_t *record, size_t len);

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
