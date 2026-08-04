#include "jpp_serial_mgr.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"
#include "esp_system.h"
#include "ff.h"
#include "cJSON.h"

#include "jpp_draw_util.h"
#include "jpp_buzzer_core.h"
#include "jpp_file_util.h"
#include "ssd1306.h"
#include "jpp_resource_budget.h"
#include "jpp_settings_screen.h"   /* JPPDOS_VERSION */
#include "jpp_nvs_util.h"
#include "jpp_backup_restore.h"
#include "jpp_app_dispatch.h"      /* s_active_sdk_context */
#include "jpp_lrv.h"
#include "sdkconfig.h"

static const char *TAG = "smp";

/* ---- Wire constants ------------------------------------------------------- */

static const uint8_t SMP_SOF[4] = {0x01u, 0x4Au, 0x50u, 0x50u};

#define SMP_PROTO_VERSION   1u

#define SMP_CMD_SESSION_START   0x00u
#define SMP_CMD_SESSION_END     0x01u
#define SMP_CMD_GET_INFO        0x02u
#define SMP_CMD_GET_LRV_DATA    0x03u
#define SMP_CMD_SET_TIME        0x04u
#define SMP_CMD_KEEPALIVE       0x05u
#define SMP_CMD_FS_LIST_DIR     0x10u
#define SMP_CMD_FS_MKDIR        0x11u
#define SMP_CMD_FS_REMOVE       0x12u
#define SMP_CMD_FS_RENAME       0x13u
#define SMP_CMD_FS_UPLOAD_BEGIN 0x14u
#define SMP_CMD_FS_UPLOAD_CHUNK 0x15u
#define SMP_CMD_FS_UPLOAD_END   0x16u
#define SMP_CMD_FS_DL_BEGIN     0x17u
#define SMP_CMD_FS_DL_CHUNK     0x18u
#define SMP_CMD_FS_DL_END       0x19u
#define SMP_CMD_APPLY_BACKUP    0x1Au
#if CONFIG_JPP_LRV_PROVISIONING
#define SMP_CMD_PROVISION_LRV   0x30u   /* manufacturing-only, build-gated */
#endif

#define SMP_ST_OK             0x00u
#define SMP_ST_ERR_DENIED     0x01u
#define SMP_ST_ERR_NOT_FOUND  0x02u
#define SMP_ST_ERR_IO         0x03u
#define SMP_ST_ERR_EXISTS     0x04u
#define SMP_ST_ERR_INVALID    0x05u
#define SMP_ST_ERR_BUSY       0x06u
#define SMP_ST_ERR_NO_SESSION 0x07u
#define SMP_ST_ERR_TRANSFER   0x08u
#define SMP_ST_ERR_OVERFLOW   0x09u
#define SMP_ST_ERR_APP_RUNNING 0x0Au

/*
 * Device-initiated event frames use the same envelope as a response
 * (SEQ|STATUS-or-EVENT|BODY) but are not sent in reply to a command. SEQ_EVENT
 * is a value the host never assigns to a real command (see jppd_upload.py's
 * _next_seq(), which skips it), so a frame carrying it can never be mistaken
 * for an ordinary response — the host tells the two apart by SEQ alone, before
 * it even looks at the second byte.
 */
#define SMP_SEQ_EVENT          0xFFu
#define SMP_EVT_SESSION_ENDED  0x01u   /* device closed the session unprompted */

/* ---- Module state -------------------------------------------------------- */

typedef enum {
    SMP_CONSENT_IDLE = 0,
    SMP_CONSENT_PENDING,
    SMP_CONSENT_BACKUP_PENDING,
} smp_consent_state_t;

typedef struct {
    FILE    *fp;
    uint32_t file_size;
    uint32_t crc32;        /* upload: running accumulator; download: pre-computed */
    uint16_t chunk_count;
    uint16_t next_chunk;   /* upload: next expected chunk_idx (stop-and-wait) */
    uint8_t  id;
    bool     is_upload;
    bool     active;
} smp_transfer_t;

#define SMP_BACKUP_FILE_MAX 8192u

static volatile bool              s_session_active  = false;
static volatile smp_consent_state_t s_consent_state = SMP_CONSENT_IDLE;
static volatile bool              s_consent_allowed = false;
static volatile int               s_consent_cursor  = 1; /* 0=Deny 1=Allow */
static SemaphoreHandle_t          s_consent_sem     = NULL;
static char                       s_backup_confirm_name[64u];
static SemaphoreHandle_t          s_tx_mutex        = NULL;
static esp_timer_handle_t         s_session_timer   = NULL;
static vprintf_like_t             s_orig_vprintf    = NULL;
static smp_transfer_t             s_xfer            = {0};
static jpp_rtc_state_t           *s_smp_rtc         = NULL;
#if CONFIG_JPP_LRV_PROVISIONING
static bool                       s_first_session_done = false;
#endif

/* Static receive and scratch buffers (one command at a time in the RX task). */
static uint8_t s_rx_payload[SMP_MAX_PAYLOAD_BYTES];
static uint8_t s_list_buf[SMP_LIST_BUF_BYTES];
static uint8_t s_dl_chunk_buf[3u + SMP_CHUNK_SIZE];

/* ---- CRC-16/CCITT-FALSE -------------------------------------------------- */

/* poly=0x1021, init=0xFFFF, refIn=false, refOut=false, xorOut=0x0000 */
static uint16_t crc16_step(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8;
    for (int b = 0; b < 8; b++) {
        crc = (crc & 0x8000u) ? ((uint16_t)(crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint16_t crc16_buf(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0u; i < len; i++) {
        crc = crc16_step(crc, data[i]);
    }
    return crc;
}

/* ---- CRC-32/ISO-HDLC (matches Python zlib.crc32) ------------------------- */

/* poly=0xEDB88320 (reflected), init=0xFFFFFFFF, refIn=refOut=true,
   xorOut=0xFFFFFFFF.  Used as a running accumulator over the *internal*
   (non-inverted) state: seed with ~0u, update per buffer, finalize with ~crc.
   NOT esp_rom_crc32_le() — the ESP32-C6 ROM's crc32_le convention does not
   match zlib here (it produced a mismatching value for byte-identical data),
   so we use an explicit, provably zlib-compatible implementation instead. */
static uint32_t crc32_buf(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}

/* ---- TX helpers ---------------------------------------------------------- */

/*
 * Log hook: grab tx_mutex before each ESP_LOG line so our binary frames do not
 * interleave with text output on the shared USB-Serial-JTAG TX path.
 */
static int smp_log_hook(const char *fmt, va_list args)
{
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    int ret = s_orig_vprintf(fmt, args);
    xSemaphoreGive(s_tx_mutex);
    return ret;
}

/*
 * Send a complete response frame without heap allocation.
 * Frame layout:
 *   SOF(4) | LEN(2 LE) | SEQ(1) | STATUS(1) | BODY(body_len) | CRC16(2 LE)
 * CRC covers: LEN(2) + SEQ(1) + STATUS(1) + BODY(body_len).
 */
static void send_response(uint8_t seq, uint8_t status,
                           const uint8_t *body, uint16_t body_len)
{
    uint16_t plen = 2u + body_len;  /* SEQ + STATUS + body */
    uint8_t hdr[8] = {
        0x01u, 0x4Au, 0x50u, 0x50u,       /* SOF */
        (uint8_t)(plen & 0xFFu),           /* LEN LSB */
        (uint8_t)(plen >> 8),              /* LEN MSB */
        seq,                               /* SEQ */
        status,                            /* STATUS */
    };

    /* CRC over [LEN_LSB, LEN_MSB, SEQ, STATUS, ...body...] */
    uint16_t crc = 0xFFFFu;
    crc = crc16_buf(crc, hdr + 4u, 4u);   /* LEN + SEQ + STATUS */
    if (body_len > 0u && body != NULL) {
        crc = crc16_buf(crc, body, body_len);
    }
    uint8_t crc_bytes[2] = {(uint8_t)(crc & 0xFFu), (uint8_t)(crc >> 8)};

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    usb_serial_jtag_write_bytes(hdr, 8u, portMAX_DELAY);
    if (body_len > 0u && body != NULL) {
        usb_serial_jtag_write_bytes(body, body_len, portMAX_DELAY);
    }
    usb_serial_jtag_write_bytes(crc_bytes, 2u, portMAX_DELAY);
    xSemaphoreGive(s_tx_mutex);
}

#define send_err(seq, st) send_response((seq), (st), NULL, 0u)

/*
 * Send an unsolicited device -> host event. Reuses the response frame shape
 * with SEQ_EVENT so it needs no new wire format, just a reserved SEQ the host
 * never assigns to a command (see SMP_SEQ_EVENT above).
 */
static void send_event(uint8_t event_type, const uint8_t *body, uint16_t body_len)
{
    send_response(SMP_SEQ_EVENT, event_type, body, body_len);
}

/* ---- Session timer ------------------------------------------------------- */

static void session_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "SMP_TIMEOUT");
    if (s_xfer.active && s_xfer.fp != NULL) {
        fclose(s_xfer.fp);
        s_xfer.fp = NULL;
    }
    s_xfer.active    = false;
    s_session_active = false;
}

static void reset_session_timer(void)
{
    esp_timer_stop(s_session_timer);
    esp_timer_start_once(s_session_timer,
                         (uint64_t)SMP_SESSION_TIMEOUT_MS * 1000u);
}

static void close_session(void)
{
    esp_timer_stop(s_session_timer);
    if (s_xfer.active && s_xfer.fp != NULL) {
        fclose(s_xfer.fp);
        s_xfer.fp = NULL;
    }
    s_xfer.active    = false;
    s_session_active = false;
    ESP_LOGI(TAG, "SMP_SESSION_CLOSED");
}

/* ---- Path validation ----------------------------------------------------- */

/* True for "/sd" itself or any path below it ("/sd/..."); rejects other
   mounts that merely share the prefix characters (e.g. "/sdx"). */
static bool sd_prefix_ok(const char *p)
{
    return strncmp(p, "/sd", 3) == 0 && (p[3] == '\0' || p[3] == '/');
}

static bool path_valid_sd(const uint8_t *body, uint16_t body_len,
                           const char **out)
{
    if (body_len == 0u || body[body_len - 1u] != '\0') { return false; }
    const char *p = (const char *)body;
    if (!sd_prefix_ok(p)) { return false; }
    *out = p;
    return true;
}

/* ---- Command handlers ---------------------------------------------------- */

static void handle_session_start(uint8_t seq, const uint8_t *body,
                                  uint16_t body_len)
{
    if (s_session_active) {
        send_err(seq, SMP_ST_ERR_BUSY);
        return;
    }
    if (s_active_sdk_context != NULL) {
        send_err(seq, SMP_ST_ERR_APP_RUNNING);
        return;
    }
    if (body_len < 1u || body[0] != SMP_PROTO_VERSION) {
        send_err(seq, SMP_ST_ERR_INVALID);
        return;
    }

#if CONFIG_JPP_LRV_PROVISIONING
    /* Manufacturing convenience: the very first session after boot on a
     * provisioning build auto-accepts (no OLED dialog), so prepare_device.py
     * can run unattended once the unit is powered on. Every session after
     * that still requires manual consent, same as production firmware. */
    if (!s_first_session_done) {
        s_first_session_done = true;
        s_consent_allowed    = true;
        ESP_LOGI(TAG, "SMP_CONSENT_AUTO (provisioning build, first session)");
    } else
#endif
    {
        /* Signal main loop to display consent dialog; block until resolved. */
        s_consent_cursor = 1;                  /* default cursor: Allow */
        s_consent_state  = SMP_CONSENT_PENDING;
        jpp_buzzer_play_async(JPP_BUZZER_SOUND_NOTIFY);
        xSemaphoreTake(s_consent_sem, portMAX_DELAY);  /* main loop gives this */
    }

    if (!s_consent_allowed) {
        ESP_LOGI(TAG, "SMP_CONSENT_DENIED");
        send_err(seq, SMP_ST_ERR_DENIED);
        return;
    }

    s_session_active = true;
    reset_session_timer();
    uint8_t resp[1] = {SMP_PROTO_VERSION};
    send_response(seq, SMP_ST_OK, resp, 1u);
    ESP_LOGI(TAG, "SMP_SESSION_STARTED");
}

static void handle_session_end(uint8_t seq)
{
    close_session();
    send_err(seq, SMP_ST_OK);
}

/*
 * GET_INFO response body:
 *   [fw_version: NUL-terminated string]
 *   [username: NUL-terminated string]
 *   [hwid: NUL-terminated string, "AA:BB:CC:DD:EE:FF" from eFuse MAC]
 *   [sd_total: 8 B LE uint64, bytes]
 *   [sd_used:  8 B LE uint64, bytes]
 *   [sd_free:  8 B LE uint64, bytes]
 *   [sd_label: NUL-terminated string, empty if SD unavailable]
 */
static void handle_get_info(uint8_t seq)
{
    /* firmware version */
    const char *ver = JPPDOS_VERSION;
    size_t ver_len = strlen(ver) + 1u;

    /* stored username */
    char username[JPP_SETTINGS_USERNAME_MAX + 1u];
    if (!jpp_nvs_get_str("jpp_user", "username", username, sizeof(username))) {
        username[0] = '\0';
    }
    size_t user_len = strlen(username) + 1u;

    /* eFuse MAC as "AA:BB:CC:DD:EE:FF" */
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    char hwid[18u + 1u];
    snprintf(hwid, sizeof(hwid), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    size_t hwid_len = strlen(hwid) + 1u;

    /* SD space */
    uint64_t sd_total = 0u, sd_free = 0u, sd_used = 0u;
    if (esp_vfs_fat_info("/sd", &sd_total, &sd_free) == ESP_OK) {
        sd_used = (sd_total >= sd_free) ? (sd_total - sd_free) : 0u;
    }

    /* SD volume label */
    char sd_label[25u] = "";
    FRESULT fr = f_getlabel("0:", sd_label, NULL);
    if (fr != FR_OK) {
        sd_label[0] = '\0';
    }
    size_t label_len = strlen(sd_label) + 1u;

    /* pack response */
    uint8_t buf[256u];
    size_t off = 0u;
    memcpy(buf + off, ver,      ver_len);   off += ver_len;
    memcpy(buf + off, username, user_len);  off += user_len;
    memcpy(buf + off, hwid,     hwid_len);  off += hwid_len;
    memcpy(buf + off, &sd_total, 8u);       off += 8u;
    memcpy(buf + off, &sd_used,  8u);       off += 8u;
    memcpy(buf + off, &sd_free,  8u);       off += 8u;
    memcpy(buf + off, sd_label, label_len); off += label_len;
    send_response(seq, SMP_ST_OK, buf, (uint16_t)off);
}

/*
 * GET_LRV_DATA response body:
 *   [cert: NUL-terminated string (≤192 B)]
 *   [cert_sig: 64 B]
 *   [device_pubkey: 32 B]
 *   [challenge: NUL-terminated string "{username}|{iso8601}" (≤128 B)]
 *   [resp_sig: 64 B]
 */
static uint8_t s_lrv_resp_buf[512u];

static void handle_get_lrv_data(uint8_t seq)
{
    if (!jpp_lrv_has_data()) {
        send_err(seq, SMP_ST_ERR_NOT_FOUND);
        return;
    }

    static jpp_lrv_data_t d;
    if (jpp_lrv_get_full_data(&d) != JPP_LRV_OK) {
        send_err(seq, SMP_ST_ERR_IO);
        return;
    }

    char challenge[JPP_LRV_CHALLENGE_MAX];
    jpp_lrv_build_challenge(s_smp_rtc, challenge, sizeof(challenge),
                            NULL, 0u, NULL, NULL);

    /* Sign the challenge. */
    uint8_t resp_sig[64] = {0};
    jpp_lrv_sign_challenge(challenge, resp_sig);

    /* Pack response buffer. */
    size_t pos = 0u;
    size_t cert_len = strnlen(d.cert, JPP_LRV_CERT_MAX - 1u) + 1u;
    size_t ch_len   = strlen(challenge) + 1u;

    if (pos + cert_len + 64u + 32u + ch_len + 64u > sizeof(s_lrv_resp_buf)) {
        send_err(seq, SMP_ST_ERR_OVERFLOW);
        return;
    }

    memcpy(s_lrv_resp_buf + pos, d.cert, cert_len);       pos += cert_len;
    memcpy(s_lrv_resp_buf + pos, d.cert_sig, 64u);        pos += 64u;
    memcpy(s_lrv_resp_buf + pos, d.device_pubkey, 32u);   pos += 32u;
    memcpy(s_lrv_resp_buf + pos, challenge, ch_len);       pos += ch_len;
    memcpy(s_lrv_resp_buf + pos, resp_sig, 64u);           pos += 64u;

    send_response(seq, SMP_ST_OK, s_lrv_resp_buf, (uint16_t)pos);
}

/*
 * SET_TIME body:
 *   [year: 2 B LE u16][month:1][day:1][weekday:1][hour:1][minute:1][second:1]
 * Updates the in-RAM RTC state and, if a DS1307 is attached, writes it to the
 * hardware too (mirrors ntp_apply() in app_main.c).
 */
static void handle_set_time(uint8_t seq, const uint8_t *body, uint16_t body_len)
{
    if (body_len != 8u) { send_err(seq, SMP_ST_ERR_INVALID); return; }

    jpp_rtc_datetime_t dt = {
        .year       = (int)((uint16_t)body[0] | ((uint16_t)body[1] << 8)),
        .month      = body[2],
        .day        = body[3],
        .weekday    = body[4],
        .hour       = body[5],
        .minute     = body[6],
        .second     = body[7],
        .subseconds = 0,
    };

    if (s_smp_rtc == NULL || !jpp_rtc_datetime_valid(&dt)) {
        send_err(seq, SMP_ST_ERR_INVALID);
        return;
    }

    jpp_rtc_set_time(s_smp_rtc, &dt);
    if (s_smp_rtc->hw_attached) {
        jpp_rtc_hw_write(s_smp_rtc, &dt);
    }
    send_err(seq, SMP_ST_OK);
    ESP_LOGI(TAG, "SMP_SET_TIME %04d-%02d-%02d %02d:%02d:%02d",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

/*
 * KEEPALIVE is a no-op the host can send during a stretch where it has
 * nothing else to say to the device (idling on user input, redrawing a UI)
 * so the session doesn't lapse after SMP_SESSION_TIMEOUT_MS. It needs no
 * handler-side logic of its own: dispatch_command() already resets the
 * inactivity timer before invoking *any* handler for a valid, session-gated
 * command, and KEEPALIVE is gated the same way (ERR_NO_SESSION with no
 * session open) so it can't be used to dodge that gate.
 */
static void handle_keepalive(uint8_t seq)
{
    send_err(seq, SMP_ST_OK);
}

static void handle_fs_list_dir(uint8_t seq, const uint8_t *body,
                                uint16_t body_len)
{
    const char *path;
    if (!path_valid_sd(body, body_len, &path)) {
        send_err(seq, SMP_ST_ERR_INVALID); return;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        send_err(seq, SMP_ST_ERR_NOT_FOUND); return;
    }

    size_t off = 2u;   /* reserve 2 bytes for entry_count */
    uint16_t count = 0u;
    struct dirent *ent;
    char child_path[512u];

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') { continue; }
        size_t nlen = strlen(ent->d_name);
        /* flags(1) + size_or_count(4) + name + NUL */
        if (off + 5u + nlen + 1u > SMP_LIST_BUF_BYTES) { break; }

        bool is_dir = (ent->d_type == DT_DIR);
        uint32_t size_or_count = 0u;

        snprintf(child_path, sizeof(child_path), "%s/%s", path, ent->d_name);
        if (is_dir) {
            /* count first-level (non-hidden) children */
            DIR *sub = opendir(child_path);
            if (sub != NULL) {
                struct dirent *se;
                while ((se = readdir(sub)) != NULL) {
                    if (se->d_name[0] != '.') { size_or_count++; }
                }
                closedir(sub);
            }
        } else {
            struct stat st;
            if (stat(child_path, &st) == 0) {
                size_or_count = (uint32_t)st.st_size;
            }
        }

        s_list_buf[off++] = is_dir ? 0x01u : 0x00u;
        s_list_buf[off++] = (uint8_t)(size_or_count);
        s_list_buf[off++] = (uint8_t)(size_or_count >> 8);
        s_list_buf[off++] = (uint8_t)(size_or_count >> 16);
        s_list_buf[off++] = (uint8_t)(size_or_count >> 24);
        memcpy(s_list_buf + off, ent->d_name, nlen + 1u);
        off += nlen + 1u;
        count++;
    }
    closedir(dir);

    s_list_buf[0] = (uint8_t)(count & 0xFFu);
    s_list_buf[1] = (uint8_t)(count >> 8);
    send_response(seq, SMP_ST_OK, s_list_buf, (uint16_t)off);
}

static void handle_fs_mkdir(uint8_t seq, const uint8_t *body, uint16_t body_len)
{
    const char *path;
    if (!path_valid_sd(body, body_len, &path)) {
        send_err(seq, SMP_ST_ERR_INVALID); return;
    }

    /* Create parent directories first. */
    jpp_make_parent_dirs(path);

    if (mkdir(path, 0755) == 0) {
        send_err(seq, SMP_ST_OK);
    } else if (errno == EEXIST) {
        send_err(seq, SMP_ST_ERR_EXISTS);
    } else {
        send_err(seq, SMP_ST_ERR_IO);
    }
}

static void handle_fs_remove(uint8_t seq, const uint8_t *body, uint16_t body_len)
{
    const char *path;
    if (!path_valid_sd(body, body_len, &path)) {
        send_err(seq, SMP_ST_ERR_INVALID); return;
    }
    if (remove(path) == 0) {
        send_err(seq, SMP_ST_OK);
    } else if (errno == ENOENT) {
        send_err(seq, SMP_ST_ERR_NOT_FOUND);
    } else {
        send_err(seq, SMP_ST_ERR_IO);
    }
}

static void handle_fs_rename(uint8_t seq, const uint8_t *body, uint16_t body_len)
{
    if (body_len < 4u) { send_err(seq, SMP_ST_ERR_INVALID); return; }

    size_t src_len = strnlen((const char *)body, body_len);
    if (src_len >= body_len - 1u) { send_err(seq, SMP_ST_ERR_INVALID); return; }

    const char *src = (const char *)body;
    const char *dst = (const char *)body + src_len + 1u;
    size_t dst_avail = body_len - src_len - 1u;
    if (strnlen(dst, dst_avail) >= dst_avail) {
        send_err(seq, SMP_ST_ERR_INVALID); return;
    }
    if (!sd_prefix_ok(src) || !sd_prefix_ok(dst)) {
        send_err(seq, SMP_ST_ERR_INVALID); return;
    }

    if (rename(src, dst) == 0) {
        send_err(seq, SMP_ST_OK);
    } else if (errno == ENOENT) {
        send_err(seq, SMP_ST_ERR_NOT_FOUND);
    } else if (errno == EEXIST) {
        send_err(seq, SMP_ST_ERR_EXISTS);
    } else {
        send_err(seq, SMP_ST_ERR_IO);
    }
}

static void handle_fs_upload_begin(uint8_t seq, const uint8_t *body,
                                    uint16_t body_len)
{
    if (s_xfer.active) { send_err(seq, SMP_ST_ERR_TRANSFER); return; }
    if (body_len < 5u)  { send_err(seq, SMP_ST_ERR_INVALID);  return; }

    uint32_t file_size = (uint32_t)body[0]
                       | ((uint32_t)body[1] << 8)
                       | ((uint32_t)body[2] << 16)
                       | ((uint32_t)body[3] << 24);

    const char *path = (const char *)body + 4u;
    uint16_t path_area = body_len - 4u;
    if (strnlen(path, path_area) >= path_area || !sd_prefix_ok(path)) {
        send_err(seq, SMP_ST_ERR_INVALID); return;
    }

    jpp_make_parent_dirs(path);

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) { send_err(seq, SMP_ST_ERR_IO); return; }

    s_xfer = (smp_transfer_t){
        .fp          = fp,
        .file_size   = file_size,
        .crc32       = ~0u,  /* CRC-32 running accumulator, init 0xFFFFFFFF */
        .chunk_count = (uint16_t)((file_size + SMP_CHUNK_SIZE - 1u) / SMP_CHUNK_SIZE),
        .next_chunk  = 0u,
        .id          = 0x00u,
        .is_upload   = true,
        .active      = true,
    };

    uint8_t resp[1] = {0x00u};  /* transfer_id */
    send_response(seq, SMP_ST_OK, resp, 1u);
}

static void handle_fs_upload_chunk(uint8_t seq, const uint8_t *body,
                                    uint16_t body_len)
{
    if (!s_xfer.active || !s_xfer.is_upload) {
        send_err(seq, SMP_ST_ERR_TRANSFER); return;
    }
    /* body: [xfer_id:1][chunk_idx:2 LE][data:...] */
    if (body_len < 4u) { send_err(seq, SMP_ST_ERR_INVALID); return; }
    if (body[0] != s_xfer.id) { send_err(seq, SMP_ST_ERR_TRANSFER); return; }

    /* Stop-and-wait ordering: accept only the expected chunk.  A retransmit of
       the chunk just written (ACK lost on the wire) is re-acknowledged without
       writing it twice; anything else aborts the transfer. */
    uint16_t chunk_idx = (uint16_t)body[1] | ((uint16_t)body[2] << 8);
    if (chunk_idx != s_xfer.next_chunk) {
        if (s_xfer.next_chunk > 0u && chunk_idx == (uint16_t)(s_xfer.next_chunk - 1u)) {
            uint8_t dup_resp[2] = {body[1], body[2]};
            send_response(seq, SMP_ST_OK, dup_resp, 2u);
            return;
        }
        fclose(s_xfer.fp);
        s_xfer.fp     = NULL;
        s_xfer.active = false;
        send_err(seq, SMP_ST_ERR_TRANSFER);
        return;
    }

    const uint8_t *data    = body + 3u;
    uint16_t       data_len = body_len - 3u;

    if (fwrite(data, 1u, data_len, s_xfer.fp) != data_len) {
        fclose(s_xfer.fp);
        s_xfer.fp     = NULL;
        s_xfer.active = false;
        send_err(seq, SMP_ST_ERR_IO);
        return;
    }
    /* Accumulate CRC-32 over data bytes as they arrive. */
    s_xfer.crc32 = crc32_buf(s_xfer.crc32, data, data_len);
    s_xfer.next_chunk++;

    /* Echo chunk_index for stop-and-wait acknowledgement. */
    uint8_t resp[2] = {body[1], body[2]};
    send_response(seq, SMP_ST_OK, resp, 2u);
}

static void handle_fs_upload_end(uint8_t seq, const uint8_t *body,
                                  uint16_t body_len)
{
    if (!s_xfer.active || !s_xfer.is_upload) {
        send_err(seq, SMP_ST_ERR_TRANSFER); return;
    }
    /* body: [xfer_id:1][crc32:4 LE] */
    if (body_len < 5u) { send_err(seq, SMP_ST_ERR_INVALID); return; }
    if (body[0] != s_xfer.id) { send_err(seq, SMP_ST_ERR_TRANSFER); return; }

    uint32_t host_crc = (uint32_t)body[1]
                      | ((uint32_t)body[2] << 8)
                      | ((uint32_t)body[3] << 16)
                      | ((uint32_t)body[4] << 24);

    fflush(s_xfer.fp);
    fclose(s_xfer.fp);
    s_xfer.fp     = NULL;
    s_xfer.active = false;

    /* Finalize CRC-32 (final XOR = 0xFFFFFFFF). */
    uint32_t final_crc = ~s_xfer.crc32;

    if (host_crc != final_crc) {
        ESP_LOGW(TAG, "SMP_UPLOAD_CRC_MISMATCH host=0x%08X dev=0x%08X",
                 (unsigned)host_crc, (unsigned)final_crc);
        send_err(seq, SMP_ST_ERR_INVALID);
        return;
    }
    send_err(seq, SMP_ST_OK);
    ESP_LOGI(TAG, "SMP_UPLOAD_OK crc=0x%08X", (unsigned)final_crc);
}

static void handle_fs_dl_begin(uint8_t seq, const uint8_t *body,
                                uint16_t body_len)
{
    if (s_xfer.active) { send_err(seq, SMP_ST_ERR_TRANSFER); return; }

    const char *path;
    if (!path_valid_sd(body, body_len, &path)) {
        send_err(seq, SMP_ST_ERR_INVALID); return;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        send_err(seq, (errno == ENOENT) ? SMP_ST_ERR_NOT_FOUND : SMP_ST_ERR_IO);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    if (sz < 0) { fclose(fp); send_err(seq, SMP_ST_ERR_IO); return; }
    uint32_t file_size = (uint32_t)sz;

    /* Compute CRC-32/ISO-HDLC over the whole file. */
    uint8_t cbuf[256];
    uint32_t crc = ~0u;
    size_t n;
    while ((n = fread(cbuf, 1u, sizeof(cbuf), fp)) > 0u) {
        crc = crc32_buf(crc, cbuf, n);
    }
    uint32_t final_crc = ~crc;
    rewind(fp);

    uint16_t chunk_count =
        (uint16_t)((file_size + SMP_CHUNK_SIZE - 1u) / SMP_CHUNK_SIZE);

    s_xfer = (smp_transfer_t){
        .fp          = fp,
        .file_size   = file_size,
        .crc32       = final_crc,
        .chunk_count = chunk_count,
        .id          = 0x00u,
        .is_upload   = false,
        .active      = true,
    };

    /* Response body: [xfer_id:1][file_size:4 LE][chunk_count:2 LE][crc32:4 LE] */
    uint8_t resp[11];
    resp[0]  = 0x00u;
    resp[1]  = (uint8_t)(file_size & 0xFFu);
    resp[2]  = (uint8_t)((file_size >>  8) & 0xFFu);
    resp[3]  = (uint8_t)((file_size >> 16) & 0xFFu);
    resp[4]  = (uint8_t)((file_size >> 24) & 0xFFu);
    resp[5]  = (uint8_t)(chunk_count & 0xFFu);
    resp[6]  = (uint8_t)(chunk_count >> 8);
    resp[7]  = (uint8_t)(final_crc & 0xFFu);
    resp[8]  = (uint8_t)((final_crc >>  8) & 0xFFu);
    resp[9]  = (uint8_t)((final_crc >> 16) & 0xFFu);
    resp[10] = (uint8_t)((final_crc >> 24) & 0xFFu);
    send_response(seq, SMP_ST_OK, resp, 11u);
}

static void handle_fs_dl_chunk(uint8_t seq, const uint8_t *body,
                                uint16_t body_len)
{
    if (!s_xfer.active || s_xfer.is_upload) {
        send_err(seq, SMP_ST_ERR_TRANSFER); return;
    }
    /* body: [xfer_id:1][chunk_idx:2 LE] */
    if (body_len < 3u) { send_err(seq, SMP_ST_ERR_INVALID); return; }
    if (body[0] != s_xfer.id) { send_err(seq, SMP_ST_ERR_TRANSFER); return; }

    uint16_t chunk_idx = (uint16_t)body[1] | ((uint16_t)body[2] << 8);
    if (chunk_idx >= s_xfer.chunk_count) {
        send_err(seq, SMP_ST_ERR_INVALID); return;
    }

    uint32_t offset = (uint32_t)chunk_idx * SMP_CHUNK_SIZE;
    if (fseek(s_xfer.fp, (long)offset, SEEK_SET) != 0) {
        send_err(seq, SMP_ST_ERR_IO); return;
    }

    /* Response body: [xfer_id:1][chunk_idx:2 LE][data:n] */
    s_dl_chunk_buf[0] = body[0];
    s_dl_chunk_buf[1] = body[1];
    s_dl_chunk_buf[2] = body[2];
    size_t n = fread(s_dl_chunk_buf + 3u, 1u, SMP_CHUNK_SIZE, s_xfer.fp);
    if (n == 0u && ferror(s_xfer.fp)) {
        send_err(seq, SMP_ST_ERR_IO); return;
    }
    send_response(seq, SMP_ST_OK, s_dl_chunk_buf, (uint16_t)(3u + n));
}

static void handle_fs_dl_end(uint8_t seq, const uint8_t *body,
                              uint16_t body_len)
{
    if (!s_xfer.active || s_xfer.is_upload) {
        send_err(seq, SMP_ST_ERR_TRANSFER); return;
    }
    if (body_len < 1u || body[0] != s_xfer.id) {
        send_err(seq, SMP_ST_ERR_TRANSFER); return;
    }
    fclose(s_xfer.fp);
    s_xfer.fp     = NULL;
    s_xfer.active = false;
    send_err(seq, SMP_ST_OK);
}

/* ---- APPLY_BACKUP -------------------------------------------------------- */

static void handle_apply_backup(uint8_t seq, const uint8_t *body,
                                 uint16_t body_len)
{
    const char *path;
    if (!path_valid_sd(body, body_len, &path)) {
        send_err(seq, SMP_ST_ERR_INVALID); return;
    }

    /* Read the backup file into a temporary heap buffer. */
    char *buf = malloc(SMP_BACKUP_FILE_MAX);
    if (buf == NULL) {
        send_err(seq, SMP_ST_ERR_IO); return;
    }

    long sz = jpp_read_file_into(path, buf, SMP_BACKUP_FILE_MAX);
    if (sz == JPP_READ_ERR_OPEN) {
        free(buf);
        send_err(seq, SMP_ST_ERR_NOT_FOUND); return;
    }
    if (sz < 0) {
        free(buf);
        send_err(seq, SMP_ST_ERR_OVERFLOW); return;
    }

    /* Quick-validate: must be a parseable backup before we bother the user. */
    char err_msg[64u];
    err_msg[0] = '\0';
    {
        /* Minimal pre-check: try parsing and looking for jppdos_backup:1. */
        cJSON *root = cJSON_Parse(buf);
        if (root == NULL) {
            free(buf);
            send_err(seq, SMP_ST_ERR_INVALID); return;
        }
        cJSON *sig = cJSON_GetObjectItem(root, "jppdos_backup");
        bool ok = cJSON_IsNumber(sig) && (int)sig->valuedouble == 1;
        cJSON_Delete(root);
        if (!ok) {
            free(buf);
            send_err(seq, SMP_ST_ERR_INVALID); return;
        }
    }

    /* Extract filename for the confirmation dialog. */
    const char *slash = strrchr(path, '/');
    const char *name  = slash ? slash + 1u : path;
    strncpy(s_backup_confirm_name, name, sizeof(s_backup_confirm_name) - 1u);
    s_backup_confirm_name[sizeof(s_backup_confirm_name) - 1u] = '\0';

    /* Show confirmation dialog on the OLED; default cursor to Deny. */
    s_consent_cursor = 0;
    s_consent_state  = SMP_CONSENT_BACKUP_PENDING;
    jpp_buzzer_play_async(JPP_BUZZER_SOUND_NOTIFY);
    xSemaphoreTake(s_consent_sem, portMAX_DELAY);

    if (!s_consent_allowed) {
        free(buf);
        send_err(seq, SMP_ST_ERR_DENIED); return;
    }

    /* Apply the backup. */
    if (!jpp_backup_apply_json(buf, err_msg, sizeof(err_msg))) {
        free(buf);
        ESP_LOGW(TAG, "APPLY_BACKUP failed: %s", err_msg);
        send_err(seq, SMP_ST_ERR_IO); return;
    }
    free(buf);

    ESP_LOGI(TAG, "APPLY_BACKUP_OK path=%s", path);
    send_err(seq, SMP_ST_OK);

    /* Brief on-screen notice before the restart. */
    ssd1306_clear();
    ssd1306_draw_string(3u, 0u, "Backup applied!", false);
    ssd1306_draw_string(4u, 0u, "Restarting...",  false);
    ssd1306_flush();
    vTaskDelay(pdMS_TO_TICKS(1500u));
    esp_restart();
}

#if CONFIG_JPP_LRV_PROVISIONING
/* ---- PROVISION_LRV (manufacturing-only, build-gated) --------------------- */

static void handle_provision_lrv(uint8_t seq, const uint8_t *body,
                                 uint16_t body_len)
{
    if (body == NULL || body_len == 0u) { send_err(seq, SMP_ST_ERR_INVALID); return; }

    jpp_lrv_result_t rc = jpp_lrv_store_identity(body, body_len);
    switch (rc) {
    case JPP_LRV_OK:
        ESP_LOGI(TAG, "PROVISION_LRV_OK (%u bytes)", (unsigned)body_len);
        send_err(seq, SMP_ST_OK);
        break;
    case JPP_LRV_ERR_EXISTS:    send_err(seq, SMP_ST_ERR_EXISTS);    break;
    case JPP_LRV_ERR_NOT_FOUND: send_err(seq, SMP_ST_ERR_NOT_FOUND); break;
    case JPP_LRV_ERR_CORRUPT:   send_err(seq, SMP_ST_ERR_INVALID);   break;
    default:                    send_err(seq, SMP_ST_ERR_IO);        break;
    }
}
#endif /* CONFIG_JPP_LRV_PROVISIONING */

/* ---- Command dispatcher -------------------------------------------------- */

static void dispatch_command(const uint8_t *payload, uint16_t plen)
{
    /* Minimum command payload: SEQ(1) + CMD(1) + FLAGS(1) = 3 bytes */
    if (plen < 3u) { return; }

    uint8_t        seq      = payload[0];
    uint8_t        cmd      = payload[1];
    /* payload[2] is FLAGS — reserved (always 0x00 in v1), ignored */
    const uint8_t *body     = payload + 3u;
    uint16_t       body_len = plen - 3u;

    /* SESSION_START is the only command allowed without an active session. */
    if (!s_session_active && cmd != SMP_CMD_SESSION_START) {
        send_err(seq, SMP_ST_ERR_NO_SESSION);
        return;
    }

    /* Reset inactivity timer on every valid command. */
    if (s_session_active) {
        reset_session_timer();
    }

    switch (cmd) {
    case SMP_CMD_SESSION_START:   handle_session_start(seq, body, body_len);    break;
    case SMP_CMD_SESSION_END:     handle_session_end(seq);                       break;
    case SMP_CMD_GET_INFO:        handle_get_info(seq);                          break;
    case SMP_CMD_GET_LRV_DATA:    handle_get_lrv_data(seq);                     break;
    case SMP_CMD_SET_TIME:        handle_set_time(seq, body, body_len);         break;
    case SMP_CMD_KEEPALIVE:       handle_keepalive(seq);                        break;
    case SMP_CMD_FS_LIST_DIR:     handle_fs_list_dir(seq, body, body_len);      break;
    case SMP_CMD_FS_MKDIR:        handle_fs_mkdir(seq, body, body_len);          break;
    case SMP_CMD_FS_REMOVE:       handle_fs_remove(seq, body, body_len);         break;
    case SMP_CMD_FS_RENAME:       handle_fs_rename(seq, body, body_len);         break;
    case SMP_CMD_FS_UPLOAD_BEGIN: handle_fs_upload_begin(seq, body, body_len);  break;
    case SMP_CMD_FS_UPLOAD_CHUNK: handle_fs_upload_chunk(seq, body, body_len);  break;
    case SMP_CMD_FS_UPLOAD_END:   handle_fs_upload_end(seq, body, body_len);    break;
    case SMP_CMD_FS_DL_BEGIN:     handle_fs_dl_begin(seq, body, body_len);      break;
    case SMP_CMD_FS_DL_CHUNK:     handle_fs_dl_chunk(seq, body, body_len);      break;
    case SMP_CMD_FS_DL_END:       handle_fs_dl_end(seq, body, body_len);        break;
    case SMP_CMD_APPLY_BACKUP:    handle_apply_backup(seq, body, body_len);     break;
#if CONFIG_JPP_LRV_PROVISIONING
    case SMP_CMD_PROVISION_LRV:   handle_provision_lrv(seq, body, body_len);    break;
#endif
    default:                      send_err(seq, SMP_ST_ERR_INVALID);             break;
    }
}

/* ---- RX task ------------------------------------------------------------- */

/*
 * usb_serial_jtag_read_bytes() returns as soon as ANY bytes are available in
 * the RX ring — it does NOT block for the full requested count the way
 * uart_read_bytes() does.  A frame payload larger than one USB packet (64 B
 * at full speed) therefore arrives in several chunks, and a single read call
 * returns only the first chunk.  Loop until the full `len` bytes are read (or
 * the overall deadline elapses) so multi-packet frames — every upload chunk —
 * are assembled correctly instead of being silently dropped as short.
 */
static int smp_read_exact(uint8_t *buf, size_t len, TickType_t ticks)
{
    size_t got = 0u;
    TickType_t start = xTaskGetTickCount();
    while (got < len) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= ticks) { break; }
        int r = usb_serial_jtag_read_bytes(buf + got, len - got, ticks - elapsed);
        if (r > 0) { got += (size_t)r; }
    }
    return (int)got;
}

static void smp_rx_task(void *arg)
{
    (void)arg;
    uint8_t byte;
    uint8_t sof_idx = 0u;

    while (true) {
        /* Scan for SOF preamble byte-by-byte. */
        while (sof_idx < 4u) {
            if (usb_serial_jtag_read_bytes(&byte, 1u, portMAX_DELAY) != 1) {
                continue;
            }
            if (byte == SMP_SOF[sof_idx]) {
                sof_idx++;
            } else if (byte == SMP_SOF[0]) {
                sof_idx = 1u;  /* restart — new potential SOF start */
            } else {
                sof_idx = 0u;
            }
        }
        sof_idx = 0u;  /* reset for next frame */

        /* Read 2-byte LEN field (little-endian). */
        uint8_t len_bytes[2];
        if (smp_read_exact(len_bytes, 2u, pdMS_TO_TICKS(1000u)) != 2) {
            continue;
        }
        uint16_t plen = (uint16_t)len_bytes[0] | ((uint16_t)len_bytes[1] << 8);

        if (plen == 0u || plen > SMP_MAX_PAYLOAD_BYTES) {
            continue;  /* drop oversized or empty frame */
        }

        /* Read payload. Large uploads may need several seconds. */
        int got = smp_read_exact(s_rx_payload, plen, pdMS_TO_TICKS(10000u));
        if (got != (int)plen) { continue; }

        /* Read 2-byte CRC. */
        uint8_t crc_bytes[2];
        if (smp_read_exact(crc_bytes, 2u, pdMS_TO_TICKS(1000u)) != 2) {
            continue;
        }
        uint16_t recv_crc = (uint16_t)crc_bytes[0] | ((uint16_t)crc_bytes[1] << 8);

        /* Verify CRC over LEN bytes + payload. */
        uint16_t calc_crc = 0xFFFFu;
        calc_crc = crc16_buf(calc_crc, len_bytes, 2u);
        calc_crc = crc16_buf(calc_crc, s_rx_payload, plen);
        if (calc_crc != recv_crc) {
            ESP_LOGW(TAG, "SMP_CRC_ERR calc=0x%04X recv=0x%04X",
                     (unsigned)calc_crc, (unsigned)recv_crc);
            continue;
        }

        dispatch_command(s_rx_payload, plen);
    }
}

/* ---- Public API ---------------------------------------------------------- */

void jpp_serial_mgr_init(void)
{
    s_consent_sem = xSemaphoreCreateBinary();
    s_tx_mutex    = xSemaphoreCreateMutex();

    /* Install the USB-Serial-JTAG driver for interrupt-driven RX/TX. This
       board's single USB-C port has no separate UART bridge chip — it is
       wired directly to the chip's native USB-Serial-JTAG peripheral, so
       that (not UART0) is the channel a host actually reaches. */
    usb_serial_jtag_driver_config_t usj_cfg = {
        .rx_buffer_size = SMP_RX_BUF_BYTES,
        .tx_buffer_size = SMP_TX_BUF_BYTES,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj_cfg));

    /* Route stdout/console output through the interrupt-driven driver we just
       installed.  Without this the USB-Serial-JTAG console keeps using its
       default direct-write VFS path, so log lines and our binary frames would
       be two independent writers to the same peripheral FIFO — under sustained
       traffic that collides and wedges usb_serial_jtag_write_bytes() forever
       (which then blocks every logger on s_tx_mutex).  With one shared driver,
       the TX mutex below fully serialises frames and log lines.
       Requires USB-Serial-JTAG to be the primary console with no secondary
       console (see sdkconfig.defaults) so nothing else drives the peripheral. */
    usb_serial_jtag_vfs_use_driver();

    /* Wrap the ESP_LOG vprintf sink so binary frame writes and log lines
       never interleave on the shared USB-Serial-JTAG TX path. */
    s_orig_vprintf = esp_log_set_vprintf(smp_log_hook);

    /* Session inactivity timer. */
    esp_timer_create_args_t ta = {
        .callback        = session_timeout_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "smp_timeout",
    };
    esp_timer_create(&ta, &s_session_timer);

    xTaskCreate(smp_rx_task, "smp_rx", SMP_TASK_STACK_BYTES, NULL,
                tskIDLE_PRIORITY + 2u, NULL);

    ESP_LOGI(TAG, "SMP_INIT_OK proto_version=%u", SMP_PROTO_VERSION);
}

void jpp_serial_mgr_handle_action(jpp_ui_action_t action)
{
    if (s_consent_state == SMP_CONSENT_PENDING ||
        s_consent_state == SMP_CONSENT_BACKUP_PENDING) {
        switch (action) {
        case JPP_UI_ACTION_LEFT:  s_consent_cursor = 0; break;
        case JPP_UI_ACTION_RIGHT: s_consent_cursor = 1; break;
        case JPP_UI_ACTION_OK:
            s_consent_allowed = (s_consent_cursor == 1);
            s_consent_state   = SMP_CONSENT_IDLE;
            xSemaphoreGive(s_consent_sem);
            break;
        default: break;
        }
    } else if (s_session_active) {
        if (action == JPP_UI_ACTION_BACK) {
            /* Tell the host before tearing down local state, so a host tool
               blocked in a read (or idling between commands) learns the
               session is gone instead of discovering it only via the next
               command's ERR_NO_SESSION or a 30 s timeout. */
            send_event(SMP_EVT_SESSION_ENDED, NULL, 0u);
            close_session();
            ESP_LOGI(TAG, "SMP_SESSION_ENDED_BY_USER");
        }
    }
}

void jpp_serial_mgr_render(void)
{
    ssd1306_clear();

    if (s_consent_state == SMP_CONSENT_PENDING) {
        jpp_draw_title("Serial manager");
        ssd1306_draw_string(2u, 0u, "Allow PC access", false);
        ssd1306_draw_string(3u, 0u, "to files and", false);
        ssd1306_draw_string(4u, 0u, "device info?", false);
        ssd1306_draw_string(6u, 0u,
                            jpp_ui_consent_selector_row(s_consent_cursor != 0),
                            false);
    } else if (s_consent_state == SMP_CONSENT_BACKUP_PENDING) {
        jpp_draw_title("Serial manager");
        ssd1306_draw_string(2u, 0u, "Apply backup?", false);
        ssd1306_draw_string(3u, 0u, s_backup_confirm_name, false);
        ssd1306_draw_string(4u, 0u, "This will restart", false);
        ssd1306_draw_string(6u, 0u,
                            jpp_ui_consent_selector_row(s_consent_cursor != 0),
                            false);
    } else if (s_session_active) {
        jpp_draw_title("Serial manager");
        ssd1306_draw_string(2u, 0u, "Session active", false);
        ssd1306_draw_string(5u, 0u, "Hold OK: end", false);
    }

    ssd1306_flush();
}

bool jpp_serial_mgr_needs_render(void)
{
    return s_consent_state != SMP_CONSENT_IDLE || s_session_active;
}

bool jpp_serial_mgr_session_active(void)
{
    return s_session_active;
}

void jpp_serial_mgr_set_rtc(jpp_rtc_state_t *rtc)
{
    s_smp_rtc = rtc;
}
