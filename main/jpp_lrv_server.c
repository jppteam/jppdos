#include "jpp_lrv_server.h"
#include "jpp_lrv.h"
#include "jpp_fileserver_core.h"
#include "jpp_wifi_init.h"

#include "esp_log.h"
#include "esp_netif.h"

#include "jpp_http_server_core.h"

#include "mbedtls/base64.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "jpp_lrv_srv";

/* Server task stack, carved out of the shared app pool by
   jpp_http_server_core.  The page builder is the deepest frame here: roughly
   6 KB of hex dumps, base64/percent-encoding scratch, and URL assembly (the
   /verify URL now carries the certificate and its signature, not just the
   response, and the URL buffers are sized generously so GCC's
   -Werror=format-truncation can prove the snprintf calls can't overflow)
   plus the LRV record itself. */
#define LRV_STACK_BYTES 10240u

/* ---- State -------------------------------------------------------------- */
static bool             s_running = false;
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
static void send_verification_page(jpp_http_conn_t *conn)
{
    jpp_lrv_data_t d;
    if (jpp_lrv_get_full_data(&d) != JPP_LRV_OK) {
        jpp_http_resp_send_err(conn, JPP_HTTP_500, "No LRV identity");
        return;
    }

    /* The challenge (which already embeds username + timestamp as signed
       text) comes from one builder call so the page, the URL, and the signed
       bytes stay consistent — no separate ts/name fields to keep in sync. */
    char challenge[JPP_LRV_CHALLENGE_MAX];
    jpp_lrv_build_challenge(s_rtc, challenge, sizeof(challenge),
                            NULL, 0u, NULL, NULL);

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

    /* Build the jppdevice.by.m4l3vi.ch/verify URL. cert + certsig travel
       verbatim (base64url) so the verifier checks the exact issued bytes with
       no reconstruction; challenge travels percent-encoded as one opaque
       string so the site never has to reassemble it from separate parts. */
    char respsig_b64[128] = {0};
    b64url_encode(resp_sig, 64u, respsig_b64, sizeof(respsig_b64));
    char certsig_b64[128] = {0};
    b64url_encode(d.cert_sig, 64u, certsig_b64, sizeof(certsig_b64));
    char cert_b64[300] = {0};
    b64url_encode((const uint8_t *)d.cert, strnlen(d.cert, sizeof(d.cert) - 1u),
                  cert_b64, sizeof(cert_b64));
    char challenge_enc[400] = {0};
    url_encode(challenge, challenge_enc, sizeof(challenge_enc));

    char certpage_url[1280];
    snprintf(certpage_url, sizeof(certpage_url),
             "https://jppdevice.by.m4l3vi.ch/verify"
             "?serial=%u"
             "&cert=%s"
             "&certsig=%s"
             "&challenge=%s"
             "&resp=%s",
             (unsigned)d.serial, cert_b64, certsig_b64,
             challenge_enc, respsig_b64);

    jpp_http_resp_set_type(conn, "text/html; charset=utf-8");

    /* HTML header + styles */
    jpp_http_resp_sendstr_chunk(conn,
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
    jpp_http_resp_sendstr_chunk(conn, hdr);

    /* LRV Certificate */
    jpp_http_resp_sendstr_chunk(conn, "<p><b>LRV Certificate (plaintext):</b></p><pre>");
    jpp_http_resp_sendstr_chunk(conn, d.cert);
    jpp_http_resp_sendstr_chunk(conn, "</pre>");

    jpp_http_resp_sendstr_chunk(conn, "<p><b>LRV Certificate (hexadecimal):</b></p><pre>");
    jpp_http_resp_sendstr_chunk(conn, cert_hex);
    jpp_http_resp_sendstr_chunk(conn, "</pre>");

    /* Certificate signature */
    jpp_http_resp_sendstr_chunk(conn, "<p><b>Certificate signature:</b></p><pre>");
    jpp_http_resp_sendstr_chunk(conn, certsig_hex);
    jpp_http_resp_sendstr_chunk(conn, "</pre>");

    /* Device public key */
    jpp_http_resp_sendstr_chunk(conn, "<p><b>Device public key:</b></p><pre>");
    jpp_http_resp_sendstr_chunk(conn, pubkey_hex);
    jpp_http_resp_sendstr_chunk(conn, "</pre><hr>");

    /* Challenge / response */
    char chdr[192];
    snprintf(chdr, sizeof(chdr),
             "<p><b>Challenge:</b> <code>%s</code></p>", challenge);
    jpp_http_resp_sendstr_chunk(conn, chdr);

    jpp_http_resp_sendstr_chunk(conn, "<p><b>Response signature:</b></p><pre>");
    jpp_http_resp_sendstr_chunk(conn, respsig_hex);
    jpp_http_resp_sendstr_chunk(conn, "</pre><hr>");

    /* Certificate page button — direct link, no user prompt needed. */
    char btn[1600];
    snprintf(btn, sizeof(btn),
             "<a class='btn' href='%s'>Open Certificate Page</a>",
             certpage_url);
    jpp_http_resp_sendstr_chunk(conn, btn);

    jpp_http_resp_sendstr_chunk(conn, "</body></html>");
    jpp_http_resp_sendstr_chunk(conn, NULL);  /* finalize */
}

/* ---- Request handler ---------------------------------------------------- */

static void lrv_dispatch(jpp_http_conn_t *conn, void *user_ctx)
{
    (void)user_ctx;
    const char *method = jpp_http_method(conn);
    /* Every path serves the same page — the device advertises one URL. */
    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        send_verification_page(conn);
    } else {
        jpp_http_resp_set_hdr(conn, "Allow", "GET, HEAD");
        jpp_http_resp_send_err(conn, JPP_HTTP_405, "Method not allowed");
    }
}

/* ---- Public API --------------------------------------------------------- */

jpp_lrv_server_result_t jpp_lrv_server_start(jpp_rtc_state_t *rtc)
{
    if (!jpp_lrv_has_data()) {
        return JPP_LRV_SERVER_ERR_NO_DATA;
    }
    if (!wifi_is_connected()) {
        return JPP_LRV_SERVER_ERR_NO_WIFI;
    }

    /* Check WebDAV isn't running.  Both servers run out of the shared app
       pool, so this is also enforced there — but the dedicated error tells the
       user what to switch off, rather than "pool busy". */
    jpp_fileserver_status_t fs_status;
    jpp_fileserver_get_status(&fs_status);
    if (fs_status.state == JPP_FILESERVER_STATE_RUNNING) {
        return JPP_LRV_SERVER_ERR_WEBDAV_RUNNING;
    }

    if (s_running) {
        return JPP_LRV_SERVER_OK;  /* already running */
    }

    get_local_ip(s_ip);

    /* s_rtc must be live before the first request can arrive. */
    s_rtc = rtc;

    jpp_http_server_config_t cfg = {
        .owner          = "lrv",
        .port           = JPP_LRV_SERVER_PORT,
        .stack_bytes    = LRV_STACK_BYTES,
        .recv_timeout_s = 15u,
        .send_timeout_s = 10u,
        .handler        = lrv_dispatch,
        .user_ctx       = NULL,
    };

    jpp_http_result_t rc = jpp_http_server_start(&cfg);
    if (rc != JPP_HTTP_OK) {
        ESP_LOGE(TAG, "start failed: %s", jpp_http_result_name(rc));
        s_rtc   = NULL;
        s_ip[0] = '\0';
        return JPP_LRV_SERVER_ERR_INTERNAL;
    }

    s_running = true;
    ESP_LOGI(TAG, "LRV server started on http://%s:%u", s_ip, JPP_LRV_SERVER_PORT);
    return JPP_LRV_SERVER_OK;
}

void jpp_lrv_server_stop(void)
{
    if (!s_running) { return; }
    jpp_http_result_t rc = jpp_http_server_stop();
    if (rc != JPP_HTTP_OK) {
        ESP_LOGE(TAG, "stop failed: %s", jpp_http_result_name(rc));
        return;  /* still running, and still holding the app pool */
    }
    s_running = false;
    s_rtc     = NULL;
    s_ip[0]   = '\0';
    ESP_LOGI(TAG, "LRV server stopped");
}

bool jpp_lrv_server_is_running(void)
{
    return s_running;
}

void jpp_lrv_server_get_addr(char *out, size_t len)
{
    if (out == NULL || len == 0u) { return; }
    if (!s_running || s_ip[0] == '\0') {
        snprintf(out, len, "(not running)");
    } else {
        snprintf(out, len, "%s:%u", s_ip, JPP_LRV_SERVER_PORT);
    }
}
