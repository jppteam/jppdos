#include "jpp_lrv_server.h"
#include "jpp_lrv.h"
#include "jpp_fileserver_core.h"
#include "jpp_wifi_init.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"

#include "mbedtls/base64.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "jpp_lrv_srv";

/* ---- State -------------------------------------------------------------- */
static httpd_handle_t  s_server   = NULL;
static jpp_rtc_state_t *s_rtc     = NULL;
static char             s_ip[16]  = {0};  /* "x.x.x.x\0" */

/* ---- Helpers ------------------------------------------------------------ */

static void get_local_ip(char ip[16])
{
    ip[0] = '\0';
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) { return; }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0u) {
        snprintf(ip, 16u, IPSTR, IP2STR(&info.ip));
    }
}

/* Approximate seconds since Unix epoch (no leap-second correction). */
static uint32_t datetime_to_unix(const jpp_rtc_datetime_t *dt)
{
    static const int mdays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};

    int y   = dt->year - 1970;
    int mon = dt->month - 1;
    int d   = dt->day - 1;

    int leaps = (dt->year - 1) / 4 - (dt->year - 1) / 100 + (dt->year - 1) / 400
              - (1969 / 4 - 1969 / 100 + 1969 / 400);

    int days  = y * 365 + leaps + mdays[mon] + d;
    if (mon > 1) {
        int yy = dt->year;
        if ((yy % 4 == 0 && yy % 100 != 0) || (yy % 400 == 0)) { days++; }
    }

    return (uint32_t)((uint64_t)days * 86400u
                      + (uint32_t)dt->hour   * 3600u
                      + (uint32_t)dt->minute * 60u
                      + (uint32_t)dt->second);
}

/* URL-safe base64 (RFC 4648 §5): replaces '+' with '-', '/' with '_'. */
static int b64url_encode(const uint8_t *src, size_t srclen, char *dst, size_t dstmax)
{
    size_t olen = 0u;
    if (mbedtls_base64_encode((unsigned char *)dst, dstmax, &olen, src, srclen) != 0) {
        return -1;
    }
    for (size_t i = 0u; i < olen; i++) {
        if (dst[i] == '+') { dst[i] = '-'; }
        else if (dst[i] == '/') { dst[i] = '_'; }
    }
    dst[olen] = '\0';
    return (int)olen;
}

/* Percent-encode a string for use in a URL query parameter value. */
static void url_encode(const char *src, char *dst, size_t dstmax)
{
    size_t j = 0u;
    for (size_t i = 0u; src[i] != '\0' && j + 4u < dstmax; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = (char)c;
        } else {
            j += (size_t)snprintf(dst + j, dstmax - j, "%%%02X", c);
        }
    }
    if (j < dstmax) { dst[j] = '\0'; } else { dst[dstmax - 1u] = '\0'; }
}

/* Stream the verification HTML page in chunks. */
static void send_verification_page(httpd_req_t *req)
{
    jpp_lrv_data_t d;
    if (jpp_lrv_get_full_data(&d) != JPP_LRV_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LRV not unlocked");
        return;
    }

    /* Username, timestamp, and challenge all come from one builder call (one
       RTC read) so the page, the URL, and the signed bytes stay consistent. */
    char username[64] = {0};
    jpp_rtc_datetime_t now = {0};
    bool has_time = false;
    char challenge[JPP_LRV_CHALLENGE_MAX];
    jpp_lrv_build_challenge(s_rtc, challenge, sizeof(challenge),
                            username, sizeof(username), &now, &has_time);
    uint32_t ts = has_time ? datetime_to_unix(&now) : 0u;

    uint8_t resp_sig[64] = {0};
    jpp_lrv_sign_challenge(challenge, resp_sig);

    /* Hex dumps for on-page display. */
    char cert_hex[sizeof(d.cert) * 3u + 32u];
    char certsig_hex[64u * 3u + 32u];
    char pubkey_hex[32u * 3u + 16u];
    char respsig_hex[64u * 3u + 32u];
    jpp_lrv_hex_format((const uint8_t *)d.cert,
                       strnlen(d.cert, sizeof(d.cert) - 1u),
                       8u, cert_hex, sizeof(cert_hex));
    jpp_lrv_hex_format(d.cert_sig,      64u, 8u, certsig_hex, sizeof(certsig_hex));
    jpp_lrv_hex_format(d.device_pubkey, 32u, 8u, pubkey_hex,  sizeof(pubkey_hex));
    jpp_lrv_hex_format(resp_sig,        64u, 8u, respsig_hex, sizeof(respsig_hex));

    /* Build the jppdevice.com certificate page URL. */
    char respsig_b64[128] = {0};
    b64url_encode(resp_sig, 64u, respsig_b64, sizeof(respsig_b64));
    char name_enc[192] = {0};
    url_encode(username, name_enc, sizeof(name_enc));

    char certpage_url[512];
    snprintf(certpage_url, sizeof(certpage_url),
             "https://jppdevice.com/lrv"
             "?ts=%lu"
             "&name=%s"
             "&serial=%u"
             "&resp=%s",
             (unsigned long)ts, name_enc,
             (unsigned)d.serial, respsig_b64);

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    /* HTML header + styles */
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>J++Device LRV</title>"
        "<style>body{font-family:monospace;max-width:640px;margin:2em auto;padding:0 1em}"
        "pre{background:#f4f4f4;padding:1em;overflow-x:auto;white-space:pre-wrap;word-break:break-all}"
        "hr{margin:1.5em 0}a.btn{display:inline-block;margin:.5em .5em .5em 0;"
        "padding:.5em 1em;background:#0066cc;color:#fff;text-decoration:none;border-radius:3px}"
        "</style></head><body>"
        "<h2>J++Device Limited Run Verification</h2>");

    /* Serial */
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "<p><b>Serial:</b> #%u</p><hr>",
             (unsigned)d.serial);
    httpd_resp_sendstr_chunk(req, hdr);

    /* LRV Certificate */
    httpd_resp_sendstr_chunk(req, "<p><b>LRV Certificate (plaintext):</b></p><pre>");
    httpd_resp_sendstr_chunk(req, d.cert);
    httpd_resp_sendstr_chunk(req, "</pre>");

    httpd_resp_sendstr_chunk(req, "<p><b>LRV Certificate (hexadecimal):</b></p><pre>");
    httpd_resp_sendstr_chunk(req, cert_hex);
    httpd_resp_sendstr_chunk(req, "</pre>");

    /* Certificate signature */
    httpd_resp_sendstr_chunk(req, "<p><b>Certificate signature:</b></p><pre>");
    httpd_resp_sendstr_chunk(req, certsig_hex);
    httpd_resp_sendstr_chunk(req, "</pre>");

    /* Device public key */
    httpd_resp_sendstr_chunk(req, "<p><b>Device public key:</b></p><pre>");
    httpd_resp_sendstr_chunk(req, pubkey_hex);
    httpd_resp_sendstr_chunk(req, "</pre><hr>");

    /* Challenge / response */
    char chdr[192];
    snprintf(chdr, sizeof(chdr),
             "<p><b>Challenge:</b> <code>%s</code></p>", challenge);
    httpd_resp_sendstr_chunk(req, chdr);

    httpd_resp_sendstr_chunk(req, "<p><b>Response signature:</b></p><pre>");
    httpd_resp_sendstr_chunk(req, respsig_hex);
    httpd_resp_sendstr_chunk(req, "</pre><hr>");

    /* Certificate page button — direct link, no user prompt needed. */
    char btn[560];
    snprintf(btn, sizeof(btn),
             "<a class='btn' href='%s'>Open Certificate Page</a>",
             certpage_url);
    httpd_resp_sendstr_chunk(req, btn);

    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);  /* finalize */
}

/* ---- URI handler -------------------------------------------------------- */

static esp_err_t handle_root_get(httpd_req_t *req)
{
    send_verification_page(req);
    return ESP_OK;
}

/* ---- Public API --------------------------------------------------------- */

jpp_lrv_server_result_t jpp_lrv_server_start(jpp_rtc_state_t *rtc)
{
    if (!jpp_lrv_is_unlocked()) {
        return JPP_LRV_SERVER_ERR_NOT_UNLOCKED;
    }
    if (!wifi_is_connected()) {
        return JPP_LRV_SERVER_ERR_NO_WIFI;
    }

    /* Check WebDAV isn't running. */
    jpp_fileserver_status_t fs_status;
    jpp_fileserver_get_status(&fs_status);
    if (fs_status.state == JPP_FILESERVER_STATE_RUNNING) {
        return JPP_LRV_SERVER_ERR_WEBDAV_RUNNING;
    }

    if (s_server != NULL) {
        return JPP_LRV_SERVER_OK;  /* already running */
    }

    get_local_ip(s_ip);

    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = JPP_LRV_SERVER_PORT;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;
    cfg.max_uri_handlers  = 2u;
    cfg.max_open_sockets  = 2u;
    cfg.stack_size        = 8192u;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        return JPP_LRV_SERVER_ERR_INTERNAL;
    }

    static const httpd_uri_t uri_all = {
        .uri = "/*", .method = HTTP_GET,
        .handler = handle_root_get, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_all);

    s_rtc = rtc;
    ESP_LOGI(TAG, "LRV server started on http://%s:%u", s_ip, JPP_LRV_SERVER_PORT);
    return JPP_LRV_SERVER_OK;
}

void jpp_lrv_server_stop(void)
{
    if (s_server == NULL) { return; }
    httpd_stop(s_server);
    s_server = NULL;
    s_rtc    = NULL;
    s_ip[0]  = '\0';
    ESP_LOGI(TAG, "LRV server stopped");
}

bool jpp_lrv_server_is_running(void)
{
    return (s_server != NULL);
}

void jpp_lrv_server_get_addr(char *out, size_t len)
{
    if (out == NULL || len == 0u) { return; }
    if (s_server == NULL || s_ip[0] == '\0') {
        snprintf(out, len, "(not running)");
    } else {
        snprintf(out, len, "%s:%u", s_ip, JPP_LRV_SERVER_PORT);
    }
}
