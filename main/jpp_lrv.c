#include "jpp_lrv.h"
#include "jpp_crypto_core.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "sodium.h"
#include "sodium/crypto_secretbox.h"
#include "sodium/crypto_generichash.h"
#include "sodium/randombytes.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "jpp_lrv";

/* ---- NVS keys (max 15 chars each) --------------------------------------- */
#define LRV_NVS_NS       "jpp_lrv"
#define LRV_KEY_ENC      "lrv_enc"       /* encrypted blob (locked state)   */
#define LRV_KEY_SERIAL   "lrv_serial"    /* uint16_t                         */
#define LRV_KEY_PUBKEY   "lrv_pubkey"    /* blob 32 B                        */
#define LRV_KEY_SECKEY   "lrv_seckey"    /* blob 64 B                        */
#define LRV_KEY_CERT     "lrv_cert"      /* blob ≤192 B                      */
#define LRV_KEY_CERT_SIG "lrv_cert_sig"  /* blob 64 B                        */
#define LRV_KEY_HWID     "lrv_hwid"      /* str ≤23 B (eFuse MAC text)       */
#define LRV_KEY_PASSWORD "lrv_password"  /* str ≤64 B (cached after unlock)  */

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
#define BLOB_MAX_SZ    (crypto_secretbox_NONCEBYTES + PT_MAX_SZ + crypto_secretbox_MACBYTES)

/* ---- Helpers ------------------------------------------------------------ */

static void derive_key(const char *password, uint8_t key[32])
{
    crypto_generichash(key, 32u,
                       (const uint8_t *)password, strlen(password),
                       NULL, 0u);
}

/* Serialise a jpp_lrv_data_t into buf; returns the written length. */
static size_t serialise(const jpp_lrv_data_t *d, uint8_t *buf, size_t bufsz)
{
    uint16_t cert_len = (uint16_t)(strnlen(d->cert, JPP_LRV_CERT_MAX - 1u) + 1u);
    size_t   total    = PT_FIXED_SZ + cert_len;
    if (bufsz < total) { return 0u; }

    memset(buf, 0, PT_FIXED_SZ);
    buf[PT_OFF_SERIAL]   = (uint8_t)(d->serial & 0xFFu);
    buf[PT_OFF_SERIAL+1] = (uint8_t)(d->serial >> 8u);
    memcpy(buf + PT_OFF_PUBKEY,  d->device_pubkey, 32u);
    memcpy(buf + PT_OFF_SECKEY,  d->device_seckey, 64u);
    memcpy(buf + PT_OFF_CERTSIG, d->cert_sig,      64u);
    strncpy((char *)(buf + PT_OFF_HWID), d->hwid, 23u);
    buf[PT_OFF_CERTLEN]   = (uint8_t)(cert_len & 0xFFu);
    buf[PT_OFF_CERTLEN+1] = (uint8_t)(cert_len >> 8u);
    memcpy(buf + PT_OFF_CERT, d->cert, cert_len);
    return total;
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

/* ---- Public API --------------------------------------------------------- */

bool jpp_lrv_has_data(void)
{
    nvs_handle_t h;
    if (nvs_open(LRV_NVS_NS, NVS_READONLY, &h) != ESP_OK) { return false; }
    size_t  enc_sz  = 0u;
    uint16_t serial = 0u;
    bool has_enc    = (nvs_get_blob(h, LRV_KEY_ENC,    NULL,    &enc_sz) == ESP_OK);
    bool has_plain  = (nvs_get_u16(h,  LRV_KEY_SERIAL, &serial) == ESP_OK);
    nvs_close(h);
    return has_enc || has_plain;
}

bool jpp_lrv_is_unlocked(void)
{
    nvs_handle_t h;
    if (nvs_open(LRV_NVS_NS, NVS_READONLY, &h) != ESP_OK) { return false; }
    uint16_t serial = 0u;
    bool unlocked = (nvs_get_u16(h, LRV_KEY_SERIAL, &serial) == ESP_OK);
    nvs_close(h);
    return unlocked;
}

jpp_lrv_result_t jpp_lrv_unlock(const char *password)
{
    if (password == NULL) { return JPP_LRV_ERR_INTERNAL; }

    /* Read encrypted blob. */
    nvs_handle_t h;
    if (nvs_open(LRV_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return JPP_LRV_ERR_NOT_FOUND;
    }
    size_t blob_sz = 0u;
    if (nvs_get_blob(h, LRV_KEY_ENC, NULL, &blob_sz) != ESP_OK) {
        nvs_close(h);
        return JPP_LRV_ERR_NOT_FOUND;
    }
    uint8_t *blob = malloc(blob_sz);
    if (blob == NULL) { nvs_close(h); return JPP_LRV_ERR_INTERNAL; }
    size_t actual = blob_sz;
    if (nvs_get_blob(h, LRV_KEY_ENC, blob, &actual) != ESP_OK) {
        free(blob);
        nvs_close(h);
        return JPP_LRV_ERR_INTERNAL;
    }
    nvs_close(h);

    /* Validate minimum blob size: nonce + MAC at minimum. */
    if (blob_sz < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + PT_FIXED_SZ + 1u) {
        free(blob);
        return JPP_LRV_ERR_CORRUPT;
    }

    /* Decrypt. */
    const uint8_t *nonce  = blob;
    const uint8_t *cipher = blob + crypto_secretbox_NONCEBYTES;
    size_t         clen   = blob_sz - crypto_secretbox_NONCEBYTES;
    size_t         ptlen  = clen - crypto_secretbox_MACBYTES;

    uint8_t *plaintext = malloc(ptlen);
    if (plaintext == NULL) { free(blob); return JPP_LRV_ERR_INTERNAL; }

    uint8_t key[32];
    derive_key(password, key);

    int rc = crypto_secretbox_open_easy(plaintext, cipher, clen, nonce, key);
    sodium_memzero(key, sizeof(key));
    free(blob);

    if (rc != 0) {
        free(plaintext);
        return JPP_LRV_ERR_WRONG_PASSWORD;
    }

    /* Deserialise. */
    jpp_lrv_data_t data;
    if (!deserialise(plaintext, ptlen, &data)) {
        sodium_memzero(plaintext, ptlen);
        free(plaintext);
        return JPP_LRV_ERR_CORRUPT;
    }
    sodium_memzero(plaintext, ptlen);
    free(plaintext);

    /* Persist decrypted fields to NVS and remove the encrypted blob. */
    if (nvs_open(LRV_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return JPP_LRV_ERR_INTERNAL;
    }

    nvs_set_u16(h, LRV_KEY_SERIAL, data.serial);

    size_t pk_sz = 32u;
    nvs_set_blob(h, LRV_KEY_PUBKEY,   data.device_pubkey, pk_sz);
    size_t sk_sz = 64u;
    nvs_set_blob(h, LRV_KEY_SECKEY,   data.device_seckey, sk_sz);
    size_t sig_sz = 64u;
    nvs_set_blob(h, LRV_KEY_CERT_SIG, data.cert_sig,      sig_sz);

    size_t cert_len = strnlen(data.cert, JPP_LRV_CERT_MAX - 1u) + 1u;
    nvs_set_blob(h, LRV_KEY_CERT, data.cert, cert_len);

    /* hwid must survive the unlock: jpp_lrv_get_encrypted_blob() re-serialises
       the full record (including hwid) when a backup re-encrypts it. */
    nvs_set_str(h, LRV_KEY_HWID, data.hwid);

    nvs_set_str(h, LRV_KEY_PASSWORD, password);

    nvs_erase_key(h, LRV_KEY_ENC);
    nvs_commit(h);
    nvs_close(h);

    uint16_t log_serial = data.serial;
    sodium_memzero(&data, sizeof(data));
    ESP_LOGI(TAG, "LRV unlocked serial=%u", (unsigned)log_serial);
    return JPP_LRV_OK;
}

void jpp_lrv_get_display_info(uint16_t *serial, char pubkey_str[16])
{
    nvs_handle_t h;
    if (nvs_open(LRV_NVS_NS, NVS_READONLY, &h) != ESP_OK) { return; }

    if (serial) {
        uint16_t v = 0u;
        nvs_get_u16(h, LRV_KEY_SERIAL, &v);
        *serial = v;
    }
    if (pubkey_str) {
        pubkey_str[0] = '\0';
        uint8_t pk[32] = {0};
        size_t  sz = sizeof(pk);
        if (nvs_get_blob(h, LRV_KEY_PUBKEY, pk, &sz) == ESP_OK && sz == 32u) {
            snprintf(pubkey_str, 16u, "%02x%02x%02x-%02x%02x%02x",
                     pk[0],  pk[1],  pk[2],
                     pk[29], pk[30], pk[31]);
        }
    }
    nvs_close(h);
}

jpp_lrv_result_t jpp_lrv_get_full_data(jpp_lrv_data_t *out)
{
    if (out == NULL) { return JPP_LRV_ERR_INTERNAL; }
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    if (nvs_open(LRV_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return JPP_LRV_ERR_NOT_FOUND;
    }

    if (nvs_get_u16(h, LRV_KEY_SERIAL, &out->serial) != ESP_OK) {
        nvs_close(h);
        return JPP_LRV_ERR_LOCKED;
    }

    size_t sz;
    sz = 32u; nvs_get_blob(h, LRV_KEY_PUBKEY,   out->device_pubkey, &sz);
    sz = 64u; nvs_get_blob(h, LRV_KEY_SECKEY,   out->device_seckey, &sz);
    sz = 64u; nvs_get_blob(h, LRV_KEY_CERT_SIG, out->cert_sig,      &sz);

    sz = JPP_LRV_CERT_MAX;
    nvs_get_blob(h, LRV_KEY_CERT, out->cert, &sz);
    out->cert[JPP_LRV_CERT_MAX - 1u] = '\0';

    sz = sizeof(out->hwid);
    nvs_get_str(h, LRV_KEY_HWID, out->hwid, &sz);
    out->hwid[JPP_LRV_HWID_MAX - 1u] = '\0';

    nvs_close(h);
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

    nvs_handle_t h;
    if (nvs_open(LRV_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return JPP_LRV_ERR_NOT_FOUND;
    }
    uint8_t seckey[64] = {0};
    size_t  sz = sizeof(seckey);
    esp_err_t rc = nvs_get_blob(h, LRV_KEY_SECKEY, seckey, &sz);
    nvs_close(h);

    if (rc != ESP_OK || sz != 64u) {
        return JPP_LRV_ERR_LOCKED;
    }

    jpp_crypto_status_t crc = jpp_crypto_sign(
        (const uint8_t *)challenge, strlen(challenge), seckey, sig);
    sodium_memzero(seckey, sizeof(seckey));

    return (crc == JPP_CRYPTO_OK) ? JPP_LRV_OK : JPP_LRV_ERR_INTERNAL;
}

jpp_lrv_result_t jpp_lrv_get_encrypted_blob(uint8_t **buf, size_t *len)
{
    if (buf == NULL || len == NULL) { return JPP_LRV_ERR_INTERNAL; }

    nvs_handle_t h;
    if (nvs_open(LRV_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return JPP_LRV_ERR_NOT_FOUND;
    }

    /* Locked state: return existing encrypted blob. */
    size_t blob_sz = 0u;
    if (nvs_get_blob(h, LRV_KEY_ENC, NULL, &blob_sz) == ESP_OK) {
        uint8_t *b = malloc(blob_sz);
        if (b == NULL) { nvs_close(h); return JPP_LRV_ERR_INTERNAL; }
        size_t actual = blob_sz;
        if (nvs_get_blob(h, LRV_KEY_ENC, b, &actual) != ESP_OK) {
            free(b);
            nvs_close(h);
            return JPP_LRV_ERR_INTERNAL;
        }
        nvs_close(h);
        *buf = b;
        *len = actual;
        return JPP_LRV_OK;
    }

    /* Unlocked state: re-encrypt with cached password. */
    uint16_t serial = 0u;
    if (nvs_get_u16(h, LRV_KEY_SERIAL, &serial) != ESP_OK) {
        nvs_close(h);
        return JPP_LRV_ERR_NOT_FOUND;
    }

    char password[JPP_LRV_PASS_MAX + 1u] = {0};
    size_t pass_sz = sizeof(password);
    if (nvs_get_str(h, LRV_KEY_PASSWORD, password, &pass_sz) != ESP_OK) {
        nvs_close(h);
        return JPP_LRV_ERR_INTERNAL;
    }
    nvs_close(h);

    jpp_lrv_data_t data;
    jpp_lrv_result_t rc = jpp_lrv_get_full_data(&data);
    if (rc != JPP_LRV_OK) { return rc; }

    uint8_t plaintext[PT_MAX_SZ];
    size_t ptlen = serialise(&data, plaintext, sizeof(plaintext));
    sodium_memzero(&data, sizeof(data));
    if (ptlen == 0u) { return JPP_LRV_ERR_INTERNAL; }

    size_t outlen = crypto_secretbox_NONCEBYTES + ptlen + crypto_secretbox_MACBYTES;
    uint8_t *out = malloc(outlen);
    if (out == NULL) { return JPP_LRV_ERR_INTERNAL; }

    uint8_t *nonce      = out;
    uint8_t *ciphertext = out + crypto_secretbox_NONCEBYTES;
    randombytes_buf(nonce, crypto_secretbox_NONCEBYTES);

    uint8_t key[32];
    derive_key(password, key);
    sodium_memzero(password, sizeof(password));

    crypto_secretbox_easy(ciphertext, plaintext, ptlen, nonce, key);
    sodium_memzero(key, sizeof(key));
    sodium_memzero(plaintext, ptlen);

    *buf = out;
    *len = outlen;
    return JPP_LRV_OK;
}

jpp_lrv_result_t jpp_lrv_store_encrypted_blob(const uint8_t *blob, size_t len)
{
    if (blob == NULL || len == 0u) { return JPP_LRV_ERR_INTERNAL; }
    if (len < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + PT_FIXED_SZ + 1u) {
        return JPP_LRV_ERR_CORRUPT;
    }

    nvs_handle_t h;
    if (nvs_open(LRV_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return JPP_LRV_ERR_INTERNAL;
    }

    /* Remove any unlocked-state keys so the namespace holds only the blob. */
    nvs_erase_key(h, LRV_KEY_SERIAL);
    nvs_erase_key(h, LRV_KEY_PUBKEY);
    nvs_erase_key(h, LRV_KEY_SECKEY);
    nvs_erase_key(h, LRV_KEY_CERT);
    nvs_erase_key(h, LRV_KEY_CERT_SIG);
    nvs_erase_key(h, LRV_KEY_HWID);
    nvs_erase_key(h, LRV_KEY_PASSWORD);

    if (nvs_set_blob(h, LRV_KEY_ENC, blob, len) != ESP_OK) {
        nvs_close(h);
        return JPP_LRV_ERR_INTERNAL;
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "LRV encrypted blob stored (%zu bytes)", len);
    return JPP_LRV_OK;
}

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
