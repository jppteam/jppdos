#include "jpp_lrv.h"
#include "jpp_crypto_core.h"
#include "jpp_eeprom_core.h"
#include "jpp_hw_config.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "sodium.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "jpp_lrv";

/* ---- EEPROM layout ------------------------------------------------------ */
/*
 * IDENTITY region (write-once) at 0x0000:
 *   magic  4 B  "JLRV"  (marks the region as written; an unprovisioned chip
 *                        reads back erased cells and fails this check)
 *   len    2 B LE  (record length)
 *   crc    2 B LE  (CRC-16/CCITT-FALSE over the record)
 *   record len B   (raw, unencrypted — see the layout below)
 *
 * This is the only identity format the firmware knows: there is no version
 * field and no other layout to fall back to.  Changing the record layout means
 * changing the magic, so chips carrying the old bytes read as "no identity".
 */
#define EE_HDR_SIZE       8u
#define EE_IDENTITY_ADDR  0x0000u
#define EE_IDENTITY_MAGIC "JLRV"

/* ---- Record serialisation layout (little-endian) ------------------------ */
/* Offsets:
 *   0  – 1   serial (uint16_t LE)
 *   2  – 33  device_pubkey (32 B)
 *  34  – 97  device_seckey (64 B)
 *  98  –161  cert_sig (64 B)
 * 162  –185  hwid (24 B, null-padded)
 * 186  –187  cert_len (uint16_t LE)
 * 188  –...  cert (cert_len bytes, including NUL)
 */
#define RC_OFF_SERIAL   0u
#define RC_OFF_PUBKEY   2u
#define RC_OFF_SECKEY  34u
#define RC_OFF_CERTSIG 98u
#define RC_OFF_HWID   162u
#define RC_OFF_CERTLEN 186u
#define RC_OFF_CERT   188u

#define RC_FIXED_SZ    RC_OFF_CERT
#define RC_MIN_SZ      (RC_FIXED_SZ + 1u)
#define RC_MAX_SZ      (RC_FIXED_SZ + JPP_LRV_CERT_MAX)

/* ---- Module state (RAM) ------------------------------------------------- */

static jpp_eeprom_state_t s_eeprom;
static jpp_lrv_data_t     s_data;          /* identity read from the EEPROM  */
static bool               s_data_ok;       /* s_data populated                */

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

/* Deserialise buf into *d; returns false if the layout is invalid. */
static bool deserialise(const uint8_t *buf, size_t len, jpp_lrv_data_t *d)
{
    if (len < RC_MIN_SZ) { return false; }
    memset(d, 0, sizeof(*d));
    d->serial = (uint16_t)(buf[RC_OFF_SERIAL] | (buf[RC_OFF_SERIAL+1] << 8));
    memcpy(d->device_pubkey, buf + RC_OFF_PUBKEY,  32u);
    memcpy(d->device_seckey, buf + RC_OFF_SECKEY,  64u);
    memcpy(d->cert_sig,      buf + RC_OFF_CERTSIG, 64u);
    strncpy(d->hwid, (const char *)(buf + RC_OFF_HWID), JPP_LRV_HWID_MAX - 1u);
    uint16_t cert_len = (uint16_t)(buf[RC_OFF_CERTLEN] | (buf[RC_OFF_CERTLEN+1] << 8));
    if (cert_len == 0u || len < RC_FIXED_SZ + cert_len) { return false; }
    size_t copy = cert_len < JPP_LRV_CERT_MAX ? cert_len : JPP_LRV_CERT_MAX - 1u;
    memcpy(d->cert, buf + RC_OFF_CERT, copy);
    d->cert[copy] = '\0';
    return true;
}

/* Read the identity record (validated against the stored CRC) into buf. */
static bool identity_read_record(uint8_t *buf, size_t bufcap, size_t *out_len)
{
    uint8_t hdr[EE_HDR_SIZE];
    if (jpp_eeprom_read(&s_eeprom, EE_IDENTITY_ADDR, hdr, sizeof(hdr)) != JPP_EEPROM_OK) {
        return false;
    }
    if (memcmp(hdr, EE_IDENTITY_MAGIC, 4u) != 0) { return false; }
    uint16_t len = (uint16_t)(hdr[4] | (hdr[5] << 8));
    uint16_t crc = (uint16_t)(hdr[6] | (hdr[7] << 8));
    if (len < RC_MIN_SZ || len > RC_MAX_SZ || len > bufcap) { return false; }
    if (jpp_eeprom_read(&s_eeprom, EE_IDENTITY_ADDR + EE_HDR_SIZE, buf, len) != JPP_EEPROM_OK) {
        return false;
    }
    if (crc16_ccitt(buf, len) != crc) {
        ESP_LOGW(TAG, "identity record CRC mismatch");
        return false;
    }
    if (out_len != NULL) { *out_len = len; }
    return true;
}

/* ---- Public API --------------------------------------------------------- */

void jpp_lrv_init(i2c_master_bus_handle_t bus)
{
    memset(&s_eeprom, 0, sizeof(s_eeprom));
    s_data_ok = false;
    memset(&s_data, 0, sizeof(s_data));

    jpp_eeprom_state_init(&s_eeprom, bus,
                          JPP_HW_EEPROM_I2C_ADDR, JPP_HW_EEPROM_SIZE_BYTES);
    if (!jpp_eeprom_present(&s_eeprom)) {
        ESP_LOGI(TAG, "no LRV EEPROM present");
        return;
    }

    uint8_t record[RC_MAX_SZ];
    size_t  record_sz = 0u;
    if (!identity_read_record(record, sizeof(record), &record_sz)) {
        ESP_LOGI(TAG, "EEPROM has no LRV identity");
        return;
    }
    if (!deserialise(record, record_sz, &s_data)) {
        ESP_LOGW(TAG, "LRV identity record malformed");
        sodium_memzero(record, sizeof(record));
        memset(&s_data, 0, sizeof(s_data));
        return;
    }
    sodium_memzero(record, sizeof(record));
    s_data_ok = true;
    ESP_LOGI(TAG, "LRV identity loaded serial=%u", (unsigned)s_data.serial);
}

bool jpp_lrv_has_data(void)
{
    return s_data_ok;
}

void jpp_lrv_get_display_info(uint16_t *serial, char pubkey_str[16])
{
    if (serial != NULL) {
        *serial = s_data_ok ? s_data.serial : 0u;
    }
    if (pubkey_str != NULL) {
        pubkey_str[0] = '\0';
        if (s_data_ok) {
            const uint8_t *pk = s_data.device_pubkey;
            snprintf(pubkey_str, 16u, "%02x%02x%02x-%02x%02x%02x",
                     pk[0], pk[1], pk[2], pk[29], pk[30], pk[31]);
        }
    }
}

jpp_lrv_result_t jpp_lrv_get_full_data(jpp_lrv_data_t *out)
{
    if (out == NULL) { return JPP_LRV_ERR_INTERNAL; }
    if (!s_data_ok) { return JPP_LRV_ERR_NOT_FOUND; }
    *out = s_data;
    return JPP_LRV_OK;
}

jpp_lrv_result_t jpp_lrv_get_run_size(uint16_t *out_run_size)
{
    if (out_run_size == NULL) { return JPP_LRV_ERR_INTERNAL; }
    if (!s_data_ok) { return JPP_LRV_ERR_NOT_FOUND; }

    /* run_size lives only as "run_size=N" text inside the certificate
       (see scripts/lrv_manufacturing.py build_cert()); no separate field. */
    const char *needle = strstr(s_data.cert, "run_size=");
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

    char iso[80] = "1970-01-01T00:00:00Z";
    if (has_time) {
        /* The RTC keeps local wall-clock time (NTP applies the timezone offset
         * before writing it — see ntp_apply() in app_main.c), but the challenge
         * timestamp carries a "Z" suffix and must therefore be UTC.  Convert
         * back by subtracting the persisted offset (NVS jpp_time/tz_h). */
        int8_t tz_h = 0;
        nvs_handle_t th;
        if (nvs_open("jpp_time", NVS_READONLY, &th) == ESP_OK) {
            nvs_get_i8(th, "tz_h", &tz_h);
            nvs_close(th);
        }
        /* days_from_civil (Howard Hinnant): calendar date -> days since epoch,
         * timezone-independent so it does not depend on the newlib TZ setting. */
        int y = now.year;
        int m = now.month;
        int d = now.day;
        int yy = y - (m <= 2);
        int era = (yy >= 0 ? yy : yy - 399) / 400;
        unsigned yoe = (unsigned)(yy - era * 400);
        unsigned doy = (unsigned)((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5) + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        long days = (long)era * 146097 + (long)doe - 719468;
        time_t utc = (time_t)days * 86400
                     + now.hour * 3600 + now.minute * 60 + now.second
                     - (time_t)tz_h * 3600;
        struct tm tm_utc;
        gmtime_r(&utc, &tm_utc);
        snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                 tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
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
    if (!s_data_ok) { return JPP_LRV_ERR_NOT_FOUND; }

    uint8_t seckey[64];
    memcpy(seckey, s_data.device_seckey, sizeof(seckey));
    jpp_crypto_status_t crc = jpp_crypto_sign(
        (const uint8_t *)challenge, strlen(challenge), seckey, sig);
    sodium_memzero(seckey, sizeof(seckey));

    return (crc == JPP_CRYPTO_OK) ? JPP_LRV_OK : JPP_LRV_ERR_INTERNAL;
}

#if CONFIG_JPP_LRV_PROVISIONING
/* True if a valid IDENTITY header is already on the chip (write-once guard). */
static bool identity_present(void)
{
    uint8_t hdr[EE_HDR_SIZE];
    if (jpp_eeprom_read(&s_eeprom, EE_IDENTITY_ADDR, hdr, sizeof(hdr)) != JPP_EEPROM_OK) {
        return false;
    }
    if (memcmp(hdr, EE_IDENTITY_MAGIC, 4u) != 0) {
        return false;
    }
    uint16_t len = (uint16_t)(hdr[4] | (hdr[5] << 8));
    return len >= RC_MIN_SZ && len <= RC_MAX_SZ;
}

jpp_lrv_result_t jpp_lrv_store_identity(const uint8_t *record, size_t len)
{
    if (record == NULL || len == 0u) { return JPP_LRV_ERR_INTERNAL; }
    if (len < RC_MIN_SZ || len > RC_MAX_SZ) { return JPP_LRV_ERR_CORRUPT; }
    if (!jpp_eeprom_present(&s_eeprom)) { return JPP_LRV_ERR_NOT_FOUND; }

    /* Reject a record the firmware could not read back as an identity. */
    jpp_lrv_data_t parsed;
    if (!deserialise(record, len, &parsed)) { return JPP_LRV_ERR_CORRUPT; }

    /* Write-once: refuse if an identity is already provisioned. */
    if (identity_present()) { return JPP_LRV_ERR_EXISTS; }

    uint8_t hdr[EE_HDR_SIZE] = {0};
    memcpy(hdr, EE_IDENTITY_MAGIC, 4u);
    hdr[4] = (uint8_t)(len & 0xFFu);
    hdr[5] = (uint8_t)(len >> 8u);
    uint16_t crc = crc16_ccitt(record, len);
    hdr[6] = (uint8_t)(crc & 0xFFu);
    hdr[7] = (uint8_t)(crc >> 8u);

    /* Record first, then header last, so a partial write never leaves a valid
       header pointing at absent/garbage record bytes. */
    if (jpp_eeprom_write(&s_eeprom, EE_IDENTITY_ADDR + EE_HDR_SIZE,
                         record, len) != JPP_EEPROM_OK) {
        return JPP_LRV_ERR_INTERNAL;
    }
    if (jpp_eeprom_write(&s_eeprom, EE_IDENTITY_ADDR, hdr, sizeof(hdr)) != JPP_EEPROM_OK) {
        return JPP_LRV_ERR_INTERNAL;
    }

    /* Verify read-back, then publish the identity to the running firmware. */
    uint8_t verify[RC_MAX_SZ];
    size_t  verify_sz = 0u;
    if (!identity_read_record(verify, sizeof(verify), &verify_sz) ||
        verify_sz != len || memcmp(verify, record, len) != 0) {
        sodium_memzero(verify, sizeof(verify));
        return JPP_LRV_ERR_INTERNAL;
    }
    sodium_memzero(verify, sizeof(verify));

    s_data    = parsed;
    s_data_ok = true;
    ESP_LOGI(TAG, "LRV identity provisioned (%zu bytes, serial=%u)",
             len, (unsigned)s_data.serial);
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
