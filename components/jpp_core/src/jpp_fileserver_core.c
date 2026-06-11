#include "../include/jpp_fileserver_core.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_random.h"

#include "jpp_heap_monitor.h"

static const char *TAG = "fileserver";

#define FILE_CHUNK_SIZE  4096u
#define FULL_PATH_MAX    300u

/* Single I/O buffer shared by handle_put and handle_get.
   httpd processes one handler at a time so no mutex needed. */
static char s_io_chunk[FILE_CHUNK_SIZE];

/* ---- Internal state ------------------------------------------------------- */

static struct {
    bool                    initialized;
    httpd_handle_t          server;
    jpp_fileserver_state_t  state;
    char                    sd_root[64];
    uint16_t                port;
    char                    password[JPP_FILESERVER_PASS_MAX + 1u];
} s_fs = {0};

/* ---- Password generation -------------------------------------------------- */

static const char PASS_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
#define PASS_CHARS_LEN 62u

static void generate_password(void)
{
    for (size_t i = 0u; i < JPP_FILESERVER_PASS_LEN; i++) {
        s_fs.password[i] = PASS_CHARS[esp_random() % PASS_CHARS_LEN];
    }
    s_fs.password[JPP_FILESERVER_PASS_LEN] = '\0';
}

/* ---- Base64 decode (for HTTP Basic Auth) ---------------------------------- */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return 0;
}

/* Decode base64 src into dst (null-terminated). Returns decoded length or -1. */
static int b64_decode(const char *src, char *dst, size_t dst_size)
{
    size_t src_len = strlen(src);
    if (src_len % 4u != 0u) { return -1; }
    if (dst_size < (src_len / 4u) * 3u + 1u) { return -1; }
    size_t out = 0u;
    for (size_t i = 0u; i < src_len; i += 4u) {
        int a = b64_val(src[i]);
        int b = b64_val(src[i + 1u]);
        int c = b64_val(src[i + 2u]);
        int d = b64_val(src[i + 3u]);
        dst[out++] = (char)((a << 2) | (b >> 4));
        if (src[i + 2u] != '=') { dst[out++] = (char)(((b & 0xf) << 4) | (c >> 2)); }
        if (src[i + 3u] != '=') { dst[out++] = (char)(((c & 0x3) << 6) | d); }
    }
    dst[out] = '\0';
    return (int)out;
}

/* ---- Auth ----------------------------------------------------------------- */

static bool check_auth(httpd_req_t *req)
{
    char hdr[64];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    if (strncmp(hdr, "Basic ", 6) != 0) { return false; }
    char creds[32];
    if (b64_decode(hdr + 6, creds, sizeof(creds)) < 0) { return false; }
    if (strncmp(creds, "jppd:", 5) != 0) { return false; }
    return strcmp(creds + 5, s_fs.password) == 0;
}

static esp_err_t deny_auth(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"WebDAV\"");
    static const char msg[] = "Unauthorized";
    httpd_resp_send(req, msg, sizeof(msg) - 1);
    return ESP_OK;
}

/* ---- Helpers -------------------------------------------------------------- */

static const char *mime_for(const char *name)
{
    const char *e = strrchr(name, '.');
    if (e == NULL) return "application/octet-stream";
    if (strcasecmp(e, ".txt")  == 0) return "text/plain";
    if (strcasecmp(e, ".py")   == 0) return "text/plain";
    if (strcasecmp(e, ".log")  == 0) return "text/plain";
    if (strcasecmp(e, ".md")   == 0) return "text/plain";
    if (strcasecmp(e, ".html") == 0) return "text/html";
    if (strcasecmp(e, ".htm")  == 0) return "text/html";
    if (strcasecmp(e, ".json") == 0) return "application/json";
    if (strcasecmp(e, ".csv")  == 0) return "text/csv";
    if (strcasecmp(e, ".xml")  == 0) return "application/xml";
    if (strcasecmp(e, ".jpg")  == 0) return "image/jpeg";
    if (strcasecmp(e, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(e, ".png")  == 0) return "image/png";
    if (strcasecmp(e, ".gif")  == 0) return "image/gif";
    if (strcasecmp(e, ".bin")  == 0) return "application/octet-stream";
    return "application/octet-stream";
}

static bool path_is_safe(const char *uri)
{
    const char *p = uri;
    while (*p != '\0') {
        if (p[0] == '.' && p[1] == '.') {
            bool at_start = (p == uri || p[-1] == '/');
            bool at_end   = (p[2] == '/' || p[2] == '\0');
            if (at_start && at_end) { return false; }
        }
        p++;
    }
    return true;
}

static bool build_path(const char *uri, char *full_path, size_t size)
{
    if (uri == NULL || uri[0] != '/' || !path_is_safe(uri)) { return false; }
    size_t root_len = strlen(s_fs.sd_root);
    size_t uri_len  = strlen(uri);
    if (root_len + uri_len + 1u > size) { return false; }
    memcpy(full_path, s_fs.sd_root, root_len);
    memcpy(full_path + root_len, uri, uri_len + 1u);
    size_t len = strlen(full_path);
    if (len > 1u && full_path[len - 1u] == '/') { full_path[len - 1u] = '\0'; }
    return true;
}

static void xml_escape(const char *src, char *buf, size_t buf_size)
{
    size_t out = 0u;
    for (size_t i = 0u; src[i] != '\0' && out + 8u < buf_size; i++) {
        char c = src[i];
        if      (c == '&') { memcpy(buf + out, "&amp;",  5); out += 5u; }
        else if (c == '<') { memcpy(buf + out, "&lt;",   4); out += 4u; }
        else if (c == '>') { memcpy(buf + out, "&gt;",   4); out += 4u; }
        else if (c == '"') { memcpy(buf + out, "&quot;", 6); out += 6u; }
        else               { buf[out++] = c; }
    }
    buf[out] = '\0';
}

/* ---- PROPFIND XML helpers ------------------------------------------------- */

static esp_err_t propfind_send_entry(httpd_req_t *req,
                                     const char  *href,
                                     bool         is_dir,
                                     long         file_size)
{
    char esc[260];
    char chunk[640];
    int  n;
    const char *display = strrchr(href, '/');
    display = (display != NULL) ? display + 1 : href;
    xml_escape(href, esc, sizeof(esc));

    if (is_dir) {
        n = snprintf(chunk, sizeof(chunk),
            "<D:response>"
              "<D:href>%s</D:href>"
              "<D:propstat>"
                "<D:prop>"
                  "<D:displayname>%s</D:displayname>"
                  "<D:resourcetype><D:collection/></D:resourcetype>"
                "</D:prop>"
                "<D:status>HTTP/1.1 200 OK</D:status>"
              "</D:propstat>"
            "</D:response>",
            esc, display);
    } else {
        n = snprintf(chunk, sizeof(chunk),
            "<D:response>"
              "<D:href>%s</D:href>"
              "<D:propstat>"
                "<D:prop>"
                  "<D:displayname>%s</D:displayname>"
                  "<D:resourcetype/>"
                  "<D:getcontentlength>%ld</D:getcontentlength>"
                  "<D:getcontenttype>%s</D:getcontenttype>"
                "</D:prop>"
                "<D:status>HTTP/1.1 200 OK</D:status>"
              "</D:propstat>"
            "</D:response>",
            esc, display, file_size, mime_for(display));
    }
    if (n <= 0) { return ESP_FAIL; }
    return httpd_resp_send_chunk(req, chunk, n);
}

/* ---- Recursive delete ----------------------------------------------------- */

static esp_err_t delete_recursive(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) { return ESP_FAIL; }
    if (!S_ISDIR(st.st_mode)) {
        return (unlink(path) == 0) ? ESP_OK : ESP_FAIL;
    }
    DIR *dir = opendir(path);
    if (dir == NULL) { return ESP_FAIL; }
    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) { continue; }
        char child[FULL_PATH_MAX];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
#pragma GCC diagnostic pop
        if (delete_recursive(child) != ESP_OK) { ok = false; }
    }
    closedir(dir);
    return (ok && rmdir(path) == 0) ? ESP_OK : ESP_FAIL;
}

/* ---- URI handlers --------------------------------------------------------- */

static esp_err_t handle_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Allow",
        "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, MOVE, PROPFIND");
    httpd_resp_set_hdr(req, "DAV",           "1");
    httpd_resp_set_hdr(req, "MS-Author-Via", "DAV");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static esp_err_t handle_propfind(httpd_req_t *req)
{
    if (!check_auth(req)) { return deny_auth(req); }

    char full_path[FULL_PATH_MAX];
    const char *uri = req->uri;
    if (!build_path(uri, full_path, sizeof(full_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }
    struct stat st;
    if (stat(full_path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }

    char depth[8] = "1";
    httpd_req_get_hdr_value_str(req, "Depth", depth, sizeof(depth));
    bool list_children = (depth[0] != '0') && S_ISDIR(st.st_mode);

    httpd_resp_set_status(req, "207 Multi-Status");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    httpd_resp_set_hdr(req, "DAV", "1");

    static const char XML_OPEN[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:multistatus xmlns:D=\"DAV:\">";

    /* Track send errors throughout — on the first failure we stop sending
       (each blocked send waits send_wait_timeout seconds before returning),
       close any open DIR, and return ESP_FAIL so httpd closes the socket
       immediately instead of waiting for the next request on a dead conn. */
    bool send_ok = (httpd_resp_send_chunk(req, XML_OPEN, sizeof(XML_OPEN) - 1) == ESP_OK);

    bool self_is_dir = S_ISDIR(st.st_mode);
    char self_href[280];
    size_t uri_len = strlen(uri);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    if (self_is_dir && (uri_len == 0u || uri[uri_len - 1u] != '/')) {
        snprintf(self_href, sizeof(self_href), "%s/", uri);
    } else {
        snprintf(self_href, sizeof(self_href), "%s", uri);
    }
#pragma GCC diagnostic pop
    if (send_ok) {
        send_ok = (propfind_send_entry(req, self_href, self_is_dir, (long)st.st_size) == ESP_OK);
    }

    if (list_children && send_ok) {
        DIR *dir = opendir(full_path);
        if (dir != NULL) {
            struct dirent *ent;
            while (send_ok && (ent = readdir(dir)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0) { continue; }
                char child_full[FULL_PATH_MAX];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
                snprintf(child_full, sizeof(child_full), "%s/%s", full_path, ent->d_name);
#pragma GCC diagnostic pop
                struct stat cst;
                if (stat(child_full, &cst) != 0) { continue; }
                bool c_is_dir = S_ISDIR(cst.st_mode);
                char child_href[280];
                const char *sep = (self_href[strlen(self_href) - 1u] == '/') ? "" : "/";
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
                if (c_is_dir) {
                    snprintf(child_href, sizeof(child_href),
                             "%s%s%s/", self_href, sep, ent->d_name);
                } else {
                    snprintf(child_href, sizeof(child_href),
                             "%s%s%s", self_href, sep, ent->d_name);
                }
#pragma GCC diagnostic pop
                send_ok = (propfind_send_entry(req, child_href, c_is_dir, (long)cst.st_size) == ESP_OK);
            }
            closedir(dir);
        }
    }

    if (!send_ok) {
        return ESP_FAIL;
    }
    static const char XML_CLOSE[] = "</D:multistatus>";
    httpd_resp_send_chunk(req, XML_CLOSE, sizeof(XML_CLOSE) - 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_get(httpd_req_t *req)
{
    if (!check_auth(req)) { return deny_auth(req); }

    char full_path[FULL_PATH_MAX];
    const char *uri = req->uri;
    if (!build_path(uri, full_path, sizeof(full_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }
    struct stat st;
    if (stat(full_path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }

    if (S_ISDIR(st.st_mode)) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        char line[300];
        int  n;
        n = snprintf(line, sizeof(line),
            "<html><head><title>SD Card \xe2\x80\x94 %s</title></head>"
            "<body><h2>%s</h2><ul>", uri, uri);
        httpd_resp_send_chunk(req, line, n);
        DIR *dir = opendir(full_path);
        if (dir != NULL) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0) { continue; }
                const char *sep = (uri[strlen(uri) - 1u] == '/') ? "" : "/";
                n = snprintf(line, sizeof(line),
                    "<li><a href=\"%s%s%s\">%s</a></li>",
                    uri, sep, ent->d_name, ent->d_name);
                httpd_resp_send_chunk(req, line, n);
            }
            closedir(dir);
        }
        const char *footer = "</ul></body></html>";
        httpd_resp_send_chunk(req, footer, (int)strlen(footer));
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    FILE *f = fopen(full_path, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open file");
        return ESP_OK;
    }
    httpd_resp_set_type(req, mime_for(full_path));
    char size_str[24];
    snprintf(size_str, sizeof(size_str), "%ld", (long)st.st_size);
    httpd_resp_set_hdr(req, "Content-Length", size_str);

    size_t bytes_read;
    bool client_gone = false;
    while ((bytes_read = fread(s_io_chunk, 1u, FILE_CHUNK_SIZE, f)) > 0u) {
        if (httpd_resp_send_chunk(req, s_io_chunk, (ssize_t)bytes_read) != ESP_OK) {
            client_gone = true;
            break;
        }
    }
    fclose(f);
    if (client_gone) {
        return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_put(httpd_req_t *req)
{
    if (!check_auth(req)) { return deny_auth(req); }

    char full_path[FULL_PATH_MAX];
    if (!build_path(req->uri, full_path, sizeof(full_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }
    struct stat st;
    if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cannot PUT to directory");
        return ESP_OK;
    }

    /* ESP-IDF httpd never sends 100 Continue automatically.  Most WebDAV
       clients (macOS Finder, etc.) send Expect: 100-continue on PUT and
       withhold the body until they receive it — causing a deadlock where
       both sides wait indefinitely.  Send the interim response on the raw
       socket before touching httpd_req_recv so the client starts sending. */
    {
        char expect[32] = {0};
        if (httpd_req_get_hdr_value_str(req, "Expect", expect, sizeof(expect)) == ESP_OK
                && strcasecmp(expect, "100-continue") == 0) {
            int sockfd = httpd_req_to_sockfd(req);
            static const char k100[] = "HTTP/1.1 100 Continue\r\n\r\n";
            send(sockfd, k100, sizeof(k100) - 1u, 0);
        }
    }

    FILE *f = fopen(full_path, "wb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
        return ESP_OK;
    }
    size_t remaining = req->content_len;
    bool recv_err  = false;  /* socket-level failure — connection must be closed */
    bool fwrite_err = false; /* SD write failure — connection is still good */
    while (remaining > 0u) {
        size_t to_read = (remaining < FILE_CHUNK_SIZE) ? remaining : FILE_CHUNK_SIZE;
        int received = httpd_req_recv(req, s_io_chunk, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            /* Transient Wi-Fi stall — keep waiting as long as the peer is
               still connected.  A real disconnect returns ECONNRESET which
               maps to HTTPD_SOCK_ERR_FAIL and falls through to recv_err. */
            continue;
        }
        if (received <= 0) { recv_err = true; break; }
        if (fwrite(s_io_chunk, 1u, (size_t)received, f) != (size_t)received) {
            fwrite_err = true; break;
        }
        remaining -= (size_t)received;
    }
    fclose(f);

    if (recv_err || fwrite_err) {
        unlink(full_path);
    }
    if (recv_err) {
        /* Per ESP-IDF docs: return ESP_FAIL on recv error so httpd closes and
           frees the socket immediately.  Returning ESP_OK on a broken socket
           leaves zombie connections that exhaust lwIP pbufs and kill pings. */
        return ESP_FAIL;
    }
    if (fwrite_err) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write error");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static esp_err_t handle_delete(httpd_req_t *req)
{
    if (!check_auth(req)) { return deny_auth(req); }

    char full_path[FULL_PATH_MAX];
    if (!build_path(req->uri, full_path, sizeof(full_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }
    struct stat st;
    if (stat(full_path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }
    if (delete_recursive(full_path) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static esp_err_t handle_move(httpd_req_t *req)
{
    if (!check_auth(req)) { return deny_auth(req); }

    char full_src[FULL_PATH_MAX];
    if (!build_path(req->uri, full_src, sizeof(full_src))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad source path");
        return ESP_OK;
    }

    char dest_hdr[300];
    if (httpd_req_get_hdr_value_str(req, "Destination", dest_hdr,
                                     sizeof(dest_hdr)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Destination");
        return ESP_OK;
    }
    /* Strip "http[s]://host" prefix to get bare URI path. */
    const char *dest_uri = dest_hdr;
    if (strncmp(dest_uri, "https://", 8) == 0) {
        dest_uri = strchr(dest_uri + 8, '/');
    } else if (strncmp(dest_uri, "http://", 7) == 0) {
        dest_uri = strchr(dest_uri + 7, '/');
    }
    if (dest_uri == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Destination");
        return ESP_OK;
    }

    char full_dst[FULL_PATH_MAX];
    if (!build_path(dest_uri, full_dst, sizeof(full_dst))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad destination path");
        return ESP_OK;
    }
    if (rename(full_src, full_dst) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Move failed");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static esp_err_t handle_mkcol(httpd_req_t *req)
{
    if (!check_auth(req)) { return deny_auth(req); }

    char full_path[FULL_PATH_MAX];
    if (!build_path(req->uri, full_path, sizeof(full_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }
    if (mkdir(full_path, 0777) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Cannot create directory");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

/* ---- Public API ----------------------------------------------------------- */

void jpp_fileserver_config_defaults(jpp_fileserver_config_t *config)
{
    if (config == NULL) { return; }
    config->port    = JPP_FILESERVER_DEFAULT_PORT;
    config->sd_root = JPP_FILESERVER_DEFAULT_ROOT;
}

jpp_fileserver_result_t jpp_fileserver_init(const jpp_fileserver_config_t *config)
{
    jpp_fileserver_config_t defaults;
    const jpp_fileserver_config_t *src;

    if (s_fs.initialized) {
        return JPP_FILESERVER_RESULT_ALREADY_INITIALIZED;
    }
    jpp_fileserver_config_defaults(&defaults);
    src = (config != NULL) ? config : &defaults;

    memset(&s_fs, 0, sizeof(s_fs));
    s_fs.port  = src->port;
    s_fs.state = JPP_FILESERVER_STATE_STOPPED;
    strncpy(s_fs.sd_root,
            (src->sd_root != NULL) ? src->sd_root : JPP_FILESERVER_DEFAULT_ROOT,
            sizeof(s_fs.sd_root) - 1u);
    s_fs.initialized = true;
    ESP_LOGI(TAG, "Initialized: root=%s port=%u", s_fs.sd_root, s_fs.port);
    return JPP_FILESERVER_RESULT_OK;
}

static jpp_fileserver_result_t start_server_impl(void)
{
    /* Event marker: heap right as the server starts.  Sustained low-heap and
       actual alloc failures are tracked globally by jpp_heap_monitor. */
    jpp_heap_monitor_log("webdav-start");
    httpd_config_t cfg      = HTTPD_DEFAULT_CONFIG();
    cfg.server_port         = s_fs.port;
    cfg.uri_match_fn        = httpd_uri_match_wildcard;
    cfg.max_uri_handlers    = 8u;
    cfg.max_open_sockets    = 2u;
    cfg.stack_size          = 8192u;
    cfg.lru_purge_enable    = true;
    cfg.recv_wait_timeout   = 30u;
    cfg.send_wait_timeout   = 10u;

    esp_err_t err = httpd_start(&s_fs.server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_fs.state       = JPP_FILESERVER_STATE_ERROR;
        s_fs.password[0] = '\0';
        return JPP_FILESERVER_RESULT_START_FAILED;
    }

    static const httpd_uri_t uri_options = {
        .uri = "/*", .method = HTTP_OPTIONS,  .handler = handle_options,  .user_ctx = NULL };
    static const httpd_uri_t uri_propfind = {
        .uri = "/*", .method = HTTP_PROPFIND, .handler = handle_propfind, .user_ctx = NULL };
    static const httpd_uri_t uri_get = {
        .uri = "/*", .method = HTTP_GET,      .handler = handle_get,      .user_ctx = NULL };
    static const httpd_uri_t uri_put = {
        .uri = "/*", .method = HTTP_PUT,      .handler = handle_put,      .user_ctx = NULL };
    static const httpd_uri_t uri_delete = {
        .uri = "/*", .method = HTTP_DELETE,   .handler = handle_delete,   .user_ctx = NULL };
    static const httpd_uri_t uri_move = {
        .uri = "/*", .method = HTTP_MOVE,     .handler = handle_move,     .user_ctx = NULL };
    static const httpd_uri_t uri_mkcol = {
        .uri = "/*", .method = HTTP_MKCOL,    .handler = handle_mkcol,    .user_ctx = NULL };

    httpd_register_uri_handler(s_fs.server, &uri_options);
    httpd_register_uri_handler(s_fs.server, &uri_propfind);
    httpd_register_uri_handler(s_fs.server, &uri_get);
    httpd_register_uri_handler(s_fs.server, &uri_put);
    httpd_register_uri_handler(s_fs.server, &uri_delete);
    httpd_register_uri_handler(s_fs.server, &uri_move);
    httpd_register_uri_handler(s_fs.server, &uri_mkcol);

    s_fs.state = JPP_FILESERVER_STATE_RUNNING;
    ESP_LOGI(TAG, "Started on port %u, serving %s", s_fs.port, s_fs.sd_root);
    return JPP_FILESERVER_RESULT_OK;
}

jpp_fileserver_result_t jpp_fileserver_start(void)
{
    if (!s_fs.initialized) { return JPP_FILESERVER_RESULT_NOT_INITIALIZED; }
    if (s_fs.state == JPP_FILESERVER_STATE_RUNNING) { return JPP_FILESERVER_RESULT_OK; }
    generate_password();
    return start_server_impl();
}

jpp_fileserver_result_t jpp_fileserver_start_with_password(const char *password)
{
    if (!s_fs.initialized) { return JPP_FILESERVER_RESULT_NOT_INITIALIZED; }
    if (s_fs.state == JPP_FILESERVER_STATE_RUNNING) { return JPP_FILESERVER_RESULT_OK; }
    if (password == NULL || password[0] == '\0') {
        generate_password();
    } else {
        strncpy(s_fs.password, password, JPP_FILESERVER_PASS_MAX);
        s_fs.password[JPP_FILESERVER_PASS_MAX] = '\0';
    }
    return start_server_impl();
}

jpp_fileserver_result_t jpp_fileserver_stop(void)
{
    if (!s_fs.initialized) {
        return JPP_FILESERVER_RESULT_NOT_INITIALIZED;
    }
    if (s_fs.state != JPP_FILESERVER_STATE_RUNNING) {
        return JPP_FILESERVER_RESULT_OK;
    }
    esp_err_t err = httpd_stop(s_fs.server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_stop failed: %s", esp_err_to_name(err));
        return JPP_FILESERVER_RESULT_STOP_FAILED;
    }
    s_fs.server      = NULL;
    s_fs.state       = JPP_FILESERVER_STATE_STOPPED;
    s_fs.password[0] = '\0';
    ESP_LOGI(TAG, "Stopped");
    jpp_heap_monitor_log("webdav-stop");
    return JPP_FILESERVER_RESULT_OK;
}

void jpp_fileserver_get_status(jpp_fileserver_status_t *status)
{
    if (status == NULL) { return; }
    status->state = s_fs.state;
    status->port  = s_fs.port;
    strncpy(status->password, s_fs.password, JPP_FILESERVER_PASS_MAX);
    status->password[JPP_FILESERVER_PASS_MAX] = '\0';

    status->ip[0] = '\0';
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0u) {
            snprintf(status->ip, sizeof(status->ip), IPSTR,
                     IP2STR(&ip_info.ip));
        }
    }
}

const char *jpp_fileserver_state_name(jpp_fileserver_state_t state)
{
    switch (state) {
    case JPP_FILESERVER_STATE_STOPPED: return "stopped";
    case JPP_FILESERVER_STATE_RUNNING: return "running";
    case JPP_FILESERVER_STATE_ERROR:   return "error";
    default:                           return "unknown";
    }
}

const char *jpp_fileserver_result_name(jpp_fileserver_result_t result)
{
    switch (result) {
    case JPP_FILESERVER_RESULT_OK:                  return "OK";
    case JPP_FILESERVER_RESULT_ALREADY_INITIALIZED: return "ALREADY_INITIALIZED";
    case JPP_FILESERVER_RESULT_NOT_INITIALIZED:     return "NOT_INITIALIZED";
    case JPP_FILESERVER_RESULT_START_FAILED:        return "START_FAILED";
    case JPP_FILESERVER_RESULT_STOP_FAILED:         return "STOP_FAILED";
    default:                                        return "UNKNOWN";
    }
}
