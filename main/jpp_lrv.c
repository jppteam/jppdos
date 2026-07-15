#include "jpp_lrv.h"
#include "jpp_crypto_core.h"
#include "jpp_eeprom_core.h"
#include "jpp_hw_config.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "sodium.h"
#include "sodium/crypto_secretbox.h"
#include "sodium/crypto_generichash.h"
#include "sodium/randombytes.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "jpp_lrv";

/* ---- EEPROM layout ------------------------------------------------------ */
/*
 * IDENTITY region (write-once) at 0x0000:
 *   magic  4 B  "LRV1"
 *   ver    1 B  0x01
 *   rsvd   1 B
 *   len    2 B LE  (encrypted blob length)
 *   crc    2 B LE  (CRC-16/CCITT-FALSE over the blob)
 *   blob   len B   (nonce || ciphertext)
 *
 * STATE region (writable, unlock cache) at 0x0800:
 *   magic  4 B  "LRVS"
 *   ver    1 B  0x01
 *   rsvd   1 B
 *   len    2 B LE  (password length)
 *   crc    2 B LE  (CRC-16/CCITT-FALSE over the password)
 *   pass   len B
 */
#define EE_HDR_SIZE       10u
#define EE_IDENTITY_ADDR  0x0000u
#define EE_STATE_ADDR     0x0800u
#define EE_IDENTITY_MAGIC "LRV1"
#define EE_STATE_MAGIC    "LRVS"
#define EE_VERSION        0x01u

/* ---- Plaintext serialisation layout (little-endian) --------------------- */
/* Offsets:
 *   0  – 1   serial (uint16_t LE)
 *   2  – 33  device_pubkey (32 B)
 *  34  – 97  device_seckey (64 B)
 *  98  –161  cert_sig (64 B)
 * 162  –185  hwid (24 B, null-padded)
 * 186  –187  cert_len (uint16_t LE)
 * 188  –...  cert (cert_len bytes, including NUL)
 */
#define PT_OFF_SERIAL   0u
#define PT_OFF_PUBKEY   2u
#define PT_OFF_SECKEY  34u
#define PT_OFF_CERTSIG 98u
#define PT_OFF_HWID   162u
#define PT_OFF_CERTLEN 186u
#define PT_OFF_CERT   188u

#define PT_FIXED_SZ    PT_OFF_CERT
#define PT_MAX_SZ      (PT_FIXED_SZ + JPP_LRV_CERT_MAX)

/* Encrypted blob = nonce || ciphertext (plaintext + MAC) */
#define BLOB_MIN_SZ    (crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + PT_FIXED_SZ + 1u)
#define BLOB_MAX_SZ    (crypto_secretbox_NONCEBYTES + PT_MAX_SZ + crypto_secretbox_MACBYTES)

/* ---- Module state (RAM) ------------------------------------------------- */

static jpp_eeprom_state_t s_eeprom;
static bool               s_identity_present;   /* valid IDENTITY header found  */
static uint16_t           s_blob_len;           /* cached identity blob length  */
static jpp_lrv_data_t     s_unlocked;           /* decrypted data (RAM only)    */
static bool               s_unlocked_ok;        /* s_unlocked populated          */

/* ---- Helpers ------------------------------------------------------------ */

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0u; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8u;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1u) ^ 0x1021u)
                                  : (uint16_t)(crc << 1u);
        }
    }
    return crc;
}

static void derive_key(const char *password, uint8_t key[32])
{
    crypto_generichash(key, 32u,
                       (const uint8_t *)password, strlen(password),
                       NULL, 0u);
}

/* Deserialise buf into *d; returns false if the layout is invalid. */
static bool deserialise(const uint8_t *buf, size_t len, jpp_lrv_data_t *d)
{
    if (len < PT_FIXED_SZ + 1u) { return false; }
    memset(d, 0, sizeof(*d));
    d->serial = (uint16_t)(buf[PT_OFF_SERIAL] | (buf[PT_OFF_SERIAL+1] << 8));
    memcpy(d->device_pubkey, buf + PT_OFF_PUBKEY,  32u);
    memcpy(d->device_seckey, buf + PT_OFF_SECKEY,  64u);
    memcpy(d->cert_sig,      buf + PT_OFF_CERTSIG, 64u);
    strncpy(d->hwid, (const char *)(buf + PT_OFF_HWID), JPP_LRV_HWID_MAX - 1u);
    uint16_t cert_len = (uint16_t)(buf[PT_OFF_CERTLEN] | (buf[PT_OFF_CERTLEN+1] << 8));
    if (cert_len == 0u || len < PT_FIXED_SZ + cert_len) { return false; }
    size_t copy = cert_len < JPP_LRV_CERT_MAX ? cert_len : JPP_LRV_CERT_MAX - 1u;
    memcpy(d->cert, buf + PT_OFF_CERT, copy);
    d->cert[copy] = '\0';
    return true;
}

/* Read + validate the IDENTITY header; on success set *out_len to the blob len. */
static bool identity_read_header(uint16_t *out_len)
{
    uint8_t hdr[EE_HDR_SIZE];
    if (jpp_eeprom_read(&s_eeprom, EE_IDENTITY_ADDR, hdr, sizeof(hdr)) != JPP_EEPROM_OK) {
        return false;
    }
    if (memcmp(hdr, EE_IDENTITY_MAGIC, 4u) != 0 || hdr[4] != EE_VERSION) {
        return false;
    }
    uint16_t len = (uint16_t)(hdr[6] | (hdr[7] << 8));
    if (len < BLOB_MIN_SZ || len > BLOB_MAX_SZ) { return false; }
    if (out_len != NULL) { *out_len = len; }
    return true;
}

/* Read the identity blob (validated against the stored CRC) into buf. */
static bool identity_read_blob(uint8_t *buf, size_t bufcap, size_t *out_len)
{
    uint8_t hdr[EE_HDR_SIZE];
    if (jpp_eeprom_read(&s_eeprom, EE_IDENTITY_ADDR, hdr, sizeof(hdr)) != JPP_EEPROM_OK) {
        return false;
    }
    if (memcmp(hdr, EE_IDENTITY_MAGIC, 4u) != 0 || hdr[4] != EE_VERSION) { return false; }
    uint16_t len = (uint16_t)(hdr[6] | (hdr[7] << 8));
    uint16_t crc = (uint16_t)(hdr[8] | (hdr[9] << 8));
    if (len < BLOB_MIN_SZ || len > BLOB_MAX_SZ || len > bufcap) { return false; }
    if (jpp_eeprom_read(&s_eeprom, EE_IDENTITY_ADDR + EE_HDR_SIZE, buf, len) != JPP_EEPROM_OK) {
        return false;
    }
    if (crc16_ccitt(buf, len) != crc) {
        ESP_LOGW(TAG, "identity blob CRC mismatch");
        return false;
    }
    if (out_len != NULL) { *out_len = len; }
    return true;
}

/* Read the cached password from the STATE region. Returns false if absent. */
static bool state_read_password(char *pw, size_t cap)
{
    uint8_t hdr[EE_HDR_SIZE];
    if (jpp_eeprom_read(&s_eeprom, EE_STATE_ADDR, hdr, sizeof(hdr)) != JPP_EEPROM_OK) {
        return false;
    }
    if (memcmp(hdr, EE_STATE_MAGIC, 4u) != 0 || hdr[4] != EE_VERSION) { return false; }
    uint16_t len = (uint16_t)(hdr[6] | (hdr[7] << 8));
    uint16_t crc = (uint16_t)(hdr[8] | (hdr[9] << 8));
    if (len == 0u || len > JPP_LRV_PASS_MAX || len >= cap) { return false; }
    if (jpp_eeprom_read(&s_eeprom, EE_STATE_ADDR + EE_HDR_SIZE,
                        (uint8_t *)pw, len) != JPP_EEPROM_OK) {
        return false;
    }
    if (crc16_ccitt((const uint8_t *)pw, len) != crc) { return false; }
    pw[len] = '\0';
    return true;
}

/* Write the cached password to the STATE region. */
static bool state_write_password(const char *pw)
{
    size_t len = strnlen(pw, JPP_LRV_PASS_MAX);
    uint8_t frame[EE_HDR_SIZE + JPP_LRV_PASS_MAX];
    memset(frame, 0, sizeof(frame));
    memcpy(frame, EE_STATE_MAGIC, 4u);
    frame[4] = EE_VERSION;
    frame[6] = (uint8_t)(len & 0xFFu);
    frame[7] = (uint8_t)(len >> 8u);
    uint16_t crc = crc16_ccitt((const uint8_t *)pw, len);
    frame[8] = (uint8_t)(crc & 0xFFu);
    frame[9] = (uint8_t)(crc >> 8u);
    memcpy(frame + EE_HDR_SIZE, pw, len);
    bool ok = jpp_eeprom_write(&s_eeprom, EE_STATE_ADDR, frame,
                               EE_HDR_SIZE + len) == JPP_EEPROM_OK;
    sodium_memzero(frame, sizeof(frame));
    return ok;
}

/* Wipe the STATE region header so no cached password is recognised. */
static void state_clear(void)
{
    uint8_t zero[EE_HDR_SIZE] = {0};
    jpp_eeprom_write(&s_eeprom, EE_STATE_ADDR, zero, sizeof(zero));
}

/* Decrypt the identity blob with password into *out. */
static jpp_lrv_result_t decrypt_identity(const char *password, jpp_lrv_data_t *out)
{
    uint8_t blob[BLOB_MAX_SZ];
    size_t  blob_sz = 0u;
    if (!identity_read_blob(blob, sizeof(blob), &blob_sz)) {
        return JPP_LRV_ERR_CORRUPT;
    }

    const uint8_t *nonce  = blob;
    const uint8_t *cipher = blob + crypto_secretbox_NONCEBYTES;
    size_t         clen   = blob_sz - crypto_secretbox_NONCEBYTES;
    size_t         ptlen  = clen - crypto_secretbox_MACBYTES;

    uint8_t *plaintext = malloc(ptlen);
    if (plaintext == NULL) { return JPP_LRV_ERR_INTERNAL; }

    uint8_t key[32];
    derive_key(password, key);
    int rc = crypto_secretbox_open_easy(plaintext, cipher, clen, nonce, key);
    sodium_memzero(key, sizeof(key));

    if (rc != 0) {
        free(plaintext);
        return JPP_LRV_ERR_WRONG_PASSWORD;
    }

    bool ok = deserialise(plaintext, ptlen, out);
    sodium_memzero(plaintext, ptlen);
    free(plaintext);
    return ok ? JPP_LRV_OK : JPP_LRV_ERR_CORRUPT;
}

/* ---- Public API --------------------------------------------------------- */

void jpp_lrv_init(i2c_master_bus_handle_t bus)
{
    memset(&s_eeprom, 0, sizeof(s_eeprom));
    s_identity_present = false;
    s_blob_len         = 0u;
    s_unlocked_ok      = false;
    memset(&s_unlocked, 0, sizeof(s_unlocked));

    jpp_eeprom_state_init(&s_eeprom, bus,
                          JPP_HW_EEPROM_I2C_ADDR, JPP_HW_EEPROM_SIZE_BYTES);
    if (!jpp_eeprom_present(&s_eeprom)) {
        ESP_LOGI(TAG, "no LRV EEPROM present");
        return;
    }

    if (!identity_read_header(&s_blob_len)) {
        ESP_LOGI(TAG, "EEPROM has no LRV identity");
        return;
    }
    s_identity_present = true;
    ESP_LOGI(TAG, "LRV identity present (blob %u B)", (unsigned)s_blob_len);

    /* Auto-unlock if a cached password is stored and still decrypts. */
    char password[JPP_LRV_PASS_MAX + 1u] = {0};
    if (state_read_password(password, sizeof(password))) {
        jpp_lrv_data_t data;
        if (decrypt_identity(password, &data) == JPP_LRV_OK) {
            s_unlocked = data;
            s_unlocked_ok = true;
            ESP_LOGI(TAG, "LRV auto-unlocked serial=%u", (unsigned)data.serial);
            sodium_memzero(&data, sizeof(data));
        } else {
            ESP_LOGW(TAG, "cached LRV password no longer valid; re-lock");
            state_clear();
        }
    }
    sodium_memzero(password, sizeof(password));
}

bool jpp_lrv_has_data(void)
{
    return s_identity_present;
}

bool jpp_lrv_is_unlocked(void)
{
    return s_unlocked_ok;
}

void jpp_lrv_relock(void)
{
    if (jpp_eeprom_present(&s_eeprom)) {
        state_clear();
    }
    sodium_memzero(&s_unlocked, sizeof(s_unlocked));
    s_unlocked_ok = false;
    ESP_LOGI(TAG, "LRV re-locked");
}

jpp_lrv_result_t jpp_lrv_unlock(const char *password)
{
    if (password == NULL) { return JPP_LRV_ERR_INTERNAL; }
    if (!s_identity_present) { return JPP_LRV_ERR_NOT_FOUND; }

    jpp_lrv_data_t data;
    jpp_lrv_result_t rc = decrypt_identity(password, &data);
    if (rc != JPP_LRV_OK) { return rc; }

    s_unlocked = data;
    s_unlocked_ok = true;

    /* Cache the password so the device stays unlocked across reboots. */
    if (!state_write_password(password)) {
        ESP_LOGW(TAG, "failed to cache LRV password (unlock is session-only)");
    }

    uint16_t log_serial = data.serial;
    sodium_memzero(&data, sizeof(data));
    ESP_LOGI(TAG, "LRV unlocked serial=%u", (unsigned)log_serial);
    return JPP_LRV_OK;
}

void jpp_lrv_get_display_info(uint16_t *serial, char pubkey_str[16])
{
    if (serial != NULL) {
        *serial = s_unlocked_ok ? s_unlocked.serial : 0u;
    }
    if (pubkey_str != NULL) {
        pubkey_str[0] = '\0';
        if (s_unlocked_ok) {
            const uint8_t *pk = s_unlocked.device_pubkey;
            snprintf(pubkey_str, 16u, "%02x%02x%02x-%02x%02x%02x",
                     pk[0], pk[1], pk[2], pk[29], pk[30], pk[31]);
        }
    }
}

jpp_lrv_result_t jpp_lrv_get_full_data(jpp_lrv_data_t *out)
{
    if (out == NULL) { return JPP_LRV_ERR_INTERNAL; }
    if (!s_unlocked_ok) { return JPP_LRV_ERR_LOCKED; }
    *out = s_unlocked;
    return JPP_LRV_OK;
}

jpp_lrv_result_t jpp_lrv_get_run_size(uint16_t *out_run_size)
{
    if (out_run_size == NULL) { return JPP_LRV_ERR_INTERNAL; }
    if (!s_unlocked_ok) { return JPP_LRV_ERR_LOCKED; }

    /* run_size lives only as "run_size=N" text inside the certificate
       (see scripts/lrv_manufacturing.py build_cert()); no separate field. */
    const char *needle = strstr(s_unlocked.cert, "run_size=");
    if (needle == NULL) { return JPP_LRV_ERR_CORRUPT; }
    unsigned int value = 0u;
    if (sscanf(needle, "run_size=%u", &value) != 1) { return JPP_LRV_ERR_CORRUPT; }
    *out_run_size = (uint16_t)value;
    return JPP_LRV_OK;
}

void jpp_lrv_build_challenge(jpp_rtc_state_t *rtc,
                              char *out, size_t out_len,
                              char *out_username, size_t username_len,
                              jpp_rtc_datetime_t *out_dt, bool *out_has_time)
{
    if (out == NULL || out_len == 0u) { return; }

    char username[64] = {0};
    nvs_handle_t h;
    if (nvs_open("jpp_user", NVS_READONLY, &h) == ESP_OK) {
        size_t ulen = sizeof(username);
        nvs_get_str(h, "username", username, &ulen);
        nvs_close(h);
    }

    jpp_rtc_datetime_t now = {0};
    bool has_time = (rtc != NULL) &&
                    (jpp_rtc_get_current(rtc, &now) == JPP_RTC_STATUS_OK);

    char iso[32] = "1970-01-01T00:00:00Z";
    if (has_time) {
        snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 now.year, now.month, now.day,
                 now.hour, now.minute, now.second);
    }
    snprintf(out, out_len, "%s|%s", username, iso);

    if (out_username != NULL && username_len > 0u) {
        strncpy(out_username, username, username_len - 1u);
        out_username[username_len - 1u] = '\0';
    }
    if (out_dt != NULL)       { *out_dt = now; }
    if (out_has_time != NULL) { *out_has_time = has_time; }
}

jpp_lrv_result_t jpp_lrv_sign_challenge(const char *challenge, uint8_t sig[64])
{
    if (challenge == NULL || sig == NULL) { return JPP_LRV_ERR_INTERNAL; }
    if (!s_unlocked_ok) { return JPP_LRV_ERR_LOCKED; }

    uint8_t seckey[64];
    memcpy(seckey, s_unlocked.device_seckey, sizeof(seckey));
    jpp_crypto_status_t crc = jpp_crypto_sign(
        (const uint8_t *)challenge, strlen(challenge), seckey, sig);
    sodium_memzero(seckey, sizeof(seckey));

    return (crc == JPP_CRYPTO_OK) ? JPP_LRV_OK : JPP_LRV_ERR_INTERNAL;
}

jpp_lrv_result_t jpp_lrv_get_encrypted_blob(uint8_t **buf, size_t *len)
{
    if (buf == NULL || len == NULL) { return JPP_LRV_ERR_INTERNAL; }
    if (!s_identity_present) { return JPP_LRV_ERR_NOT_FOUND; }

    uint8_t *b = malloc(BLOB_MAX_SZ);
    if (b == NULL) { return JPP_LRV_ERR_INTERNAL; }
    size_t blob_sz = 0u;
    if (!identity_read_blob(b, BLOB_MAX_SZ, &blob_sz)) {
        free(b);
        return JPP_LRV_ERR_CORRUPT;
    }
    *buf = b;
    *len = blob_sz;
    return JPP_LRV_OK;
}

#if CONFIG_JPP_LRV_PROVISIONING
jpp_lrv_result_t jpp_lrv_store_encrypted_blob(const uint8_t *blob, size_t len)
{
    if (blob == NULL || len == 0u) { return JPP_LRV_ERR_INTERNAL; }
    if (len < BLOB_MIN_SZ || len > BLOB_MAX_SZ) { return JPP_LRV_ERR_CORRUPT; }
    if (!jpp_eeprom_present(&s_eeprom)) { return JPP_LRV_ERR_NOT_FOUND; }

    /* Write-once: refuse if an identity is already provisioned. */
    if (identity_read_header(NULL)) { return JPP_LRV_ERR_EXISTS; }

    uint8_t hdr[EE_HDR_SIZE] = {0};
    memcpy(hdr, EE_IDENTITY_MAGIC, 4u);
    hdr[4] = EE_VERSION;
    hdr[6] = (uint8_t)(len & 0xFFu);
    hdr[7] = (uint8_t)(len >> 8u);
    uint16_t crc = crc16_ccitt(blob, len);
    hdr[8] = (uint8_t)(crc & 0xFFu);
    hdr[9] = (uint8_t)(crc >> 8u);

    /* Blob first, then header last, so a partial write never leaves a valid
       header pointing at absent/garbage blob bytes. */
    if (jpp_eeprom_write(&s_eeprom, EE_IDENTITY_ADDR + EE_HDR_SIZE,
                         blob, len) != JPP_EEPROM_OK) {
        return JPP_LRV_ERR_INTERNAL;
    }
    if (jpp_eeprom_write(&s_eeprom, EE_IDENTITY_ADDR, hdr, sizeof(hdr)) != JPP_EEPROM_OK) {
        return JPP_LRV_ERR_INTERNAL;
    }

    /* Verify read-back. */
    if (!identity_read_header(&s_blob_len)) { return JPP_LRV_ERR_INTERNAL; }
    s_identity_present = true;
    ESP_LOGI(TAG, "LRV identity provisioned (%zu bytes)", len);
    return JPP_LRV_OK;
}
#endif /* CONFIG_JPP_LRV_PROVISIONING */

void jpp_lrv_hex_format(const uint8_t *data, size_t len,
                        size_t bytes_per_row, char *buf, size_t buf_len)
{
    size_t pos = 0u;
    for (size_t i = 0u; i < len; i++) {
        if (bytes_per_row > 0u && i > 0u) {
            char sep = (i % bytes_per_row == 0u) ? '\n' : ' ';
            if (pos + 1u < buf_len) { buf[pos++] = sep; }
        }
        if (pos + 2u < buf_len) {
            pos += (size_t)snprintf(buf + pos, buf_len - pos, "%02X", data[i]);
        }
    }
    if (pos < buf_len) { buf[pos] = '\0'; }
}
