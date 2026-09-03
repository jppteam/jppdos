#include "../include/jpp_fileserver_core.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"

#include "jpp_heap_monitor.h"
#include "../include/jpp_http_server_core.h"

static const char *TAG = "fileserver";

#define FULL_PATH_MAX    300u

/* The server task stack and the (much larger) file I/O buffer are carved out of
   the shared app pool by jpp_http_server_core — this module keeps no bulk
   buffers of its own.  8 KB matches what the old httpd task was given; the
   recursive delete is the deepest thing that runs on it. */
#define WEBDAV_STACK_BYTES 8192u

/* ---- Internal state ------------------------------------------------------- */

static struct {
    bool                    initialized;
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

static bool check_auth(jpp_http_conn_t *conn)
{
    const char *hdr = jpp_http_header(conn, "Authorization");
    if (hdr == NULL) { return false; }
    if (strncmp(hdr, "Basic ", 6) != 0) { return false; }
    char creds[32];
    if (b64_decode(hdr + 6, creds, sizeof(creds)) < 0) { return false; }
    if (strncmp(creds, "jppd:", 5) != 0) { return false; }
    return strcmp(creds + 5, s_fs.password) == 0;
}

static void deny_auth(jpp_http_conn_t *conn)
{
    jpp_http_resp_set_status(conn, JPP_HTTP_401);
    jpp_http_resp_set_hdr(conn, "WWW-Authenticate", "Basic realm=\"WebDAV\"");
    jpp_http_resp_set_type(conn, "text/plain");
    jpp_http_resp_send(conn, "Unauthorized", -1);
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

static bool propfind_send_entry(jpp_http_conn_t *conn,
                                const char      *href,
                                bool             is_dir,
                                long             file_size)
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
    if (n <= 0) { return false; }
    return jpp_http_resp_send_chunk(conn, chunk, n);
}

/* ---- Recursive delete ----------------------------------------------------- */

static bool delete_recursive(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) { return false; }
    if (!S_ISDIR(st.st_mode)) {
        return (unlink(path) == 0);
    }
    DIR *dir = opendir(path);
    if (dir == NULL) { return false; }
    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) { continue; }
        char child[FULL_PATH_MAX];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
#pragma GCC diagnostic pop
        if (!delete_recursive(child)) { ok = false; }
    }
    closedir(dir);
    return (ok && rmdir(path) == 0);
}

/* ---- Method handlers ------------------------------------------------------ */

static void handle_options(jpp_http_conn_t *conn)
{
    jpp_http_resp_set_hdr(conn, "Allow",
        "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, MOVE, PROPFIND");
    jpp_http_resp_set_hdr(conn, "DAV",           "1");
    jpp_http_resp_set_hdr(conn, "MS-Author-Via", "DAV");
    jpp_http_resp_send(conn, NULL, 0);
}

static void handle_propfind(jpp_http_conn_t *conn)
{
    char full_path[FULL_PATH_MAX];
    const char *uri = jpp_http_uri(conn);
    if (!build_path(uri, full_path, sizeof(full_path))) {
        jpp_http_resp_send_err(conn, JPP_HTTP_400, "Bad path");
        return;
    }
    struct stat st;
    if (stat(full_path, &st) != 0) {
        jpp_http_resp_send_err(conn, JPP_HTTP_404, "Not found");
        return;
    }

    const char *depth = jpp_http_header(conn, "Depth");
    bool list_children = (depth == NULL || depth[0] != '0') && S_ISDIR(st.st_mode);

    jpp_http_resp_set_status(conn, JPP_HTTP_207);
    jpp_http_resp_set_type(conn, "application/xml; charset=utf-8");
    jpp_http_resp_set_hdr(conn, "DAV", "1");

    static const char XML_OPEN[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:multistatus xmlns:D=\"DAV:\">";

    /* Track send errors throughout — on the first failure we stop sending
       (each blocked send waits out the send timeout before returning) and
       close any open DIR.  Leaving the response unfinished is what tells the
       server core to drop the connection instead of waiting for a next
       request on a dead one. */
    bool send_ok = jpp_http_resp_send_chunk(conn, XML_OPEN, sizeof(XML_OPEN) - 1);

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
        send_ok = propfind_send_entry(conn, self_href, self_is_dir, (long)st.st_size);
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
                send_ok = propfind_send_entry(conn, child_href, c_is_dir, (long)cst.st_size);
            }
            closedir(dir);
        }
    }

    if (!send_ok) { return; }
    static const char XML_CLOSE[] = "</D:multistatus>";
    jpp_http_resp_send_chunk(conn, XML_CLOSE, sizeof(XML_CLOSE) - 1);
    jpp_http_resp_send_chunk(conn, NULL, 0);
}

static void handle_get(jpp_http_conn_t *conn)
{
    char full_path[FULL_PATH_MAX];
    const char *uri = jpp_http_uri(conn);
    if (!build_path(uri, full_path, sizeof(full_path))) {
        jpp_http_resp_send_err(conn, JPP_HTTP_400, "Bad path");
        return;
    }
    struct stat st;
    if (stat(full_path, &st) != 0) {
        jpp_http_resp_send_err(conn, JPP_HTTP_404, "Not found");
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        jpp_http_resp_set_type(conn, "text/html; charset=utf-8");
        char line[300];
        int  n;
        n = snprintf(line, sizeof(line),
            "<html><head><title>SD Card \xe2\x80\x94 %s</title></head>"
            "<body><h2>%s</h2><ul>", uri, uri);
        bool send_ok = jpp_http_resp_send_chunk(conn, line, n);
        DIR *dir = opendir(full_path);
        if (dir != NULL) {
            struct dirent *ent;
            while (send_ok && (ent = readdir(dir)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0) { continue; }
                const char *sep = (uri[strlen(uri) - 1u] == '/') ? "" : "/";
                n = snprintf(line, sizeof(line),
                    "<li><a href=\"%s%s%s\">%s</a></li>",
                    uri, sep, ent->d_name, ent->d_name);
                send_ok = jpp_http_resp_send_chunk(conn, line, n);
            }
            closedir(dir);
        }
        if (!send_ok) { return; }
        jpp_http_resp_sendstr_chunk(conn, "</ul></body></html>");
        jpp_http_resp_send_chunk(conn, NULL, 0);
        return;
    }

    /* Files stream straight off the SD card through the pool's I/O buffer.
       Raw open/read rather than fopen/fread: no stdio layer to copy through,
       and the buffer is far larger than any stdio buffer would be. */
    int fd = open(full_path, O_RDONLY);
    if (fd < 0) {
        jpp_http_resp_send_err(conn, JPP_HTTP_500, "Cannot open file");
        return;
    }
    jpp_http_resp_set_type(conn, mime_for(full_path));
    if (!jpp_http_resp_begin(conn, (size_t)st.st_size)) {
        close(fd);
        return;
    }

    size_t   io_size = 0u;
    uint8_t *io      = jpp_http_io_buf(conn, &io_size);
    ssize_t  got;
    while ((got = read(fd, io, io_size)) > 0) {
        if (!jpp_http_resp_write(conn, io, (size_t)got)) { break; }
    }
    close(fd);
    jpp_http_resp_finish(conn);
}

static void handle_put(jpp_http_conn_t *conn)
{
    char full_path[FULL_PATH_MAX];
    if (!build_path(jpp_http_uri(conn), full_path, sizeof(full_path))) {
        jpp_http_resp_send_err(conn, JPP_HTTP_400, "Bad path");
        return;
    }
    struct stat st;
    if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        jpp_http_resp_send_err(conn, JPP_HTTP_400, "Cannot PUT to directory");
        return;
    }

    /* Most WebDAV clients (macOS Finder, etc.) send Expect: 100-continue on
       PUT and withhold the body until they receive it — without the interim
       response both sides wait indefinitely. */
    const char *expect = jpp_http_header(conn, "Expect");
    if (expect != NULL && strcasecmp(expect, "100-continue") == 0) {
        jpp_http_send_continue(conn);
    }

    int fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        jpp_http_resp_send_err(conn, JPP_HTTP_500, "Cannot create file");
        return;
    }

    size_t   io_size = 0u;
    uint8_t *io      = jpp_http_io_buf(conn, &io_size);
    size_t remaining = jpp_http_content_len(conn);
    bool recv_err   = false;  /* socket-level failure — connection must close */
    bool write_err  = false;  /* SD write failure — connection is still good  */
    while (remaining > 0u) {
        size_t to_read = (remaining < io_size) ? remaining : io_size;
        int received = jpp_http_recv(conn, io, to_read);
        if (received == 0) {
            /* Transient Wi-Fi stall — keep waiting as long as the peer is
               still connected.  A real disconnect returns -1. */
            continue;
        }
        if (received < 0) { recv_err = true; break; }
        if (write(fd, io, (size_t)received) != (ssize_t)received) {
            write_err = true; break;
        }
        remaining -= (size_t)received;
    }
    close(fd);

    if (recv_err || write_err) {
        unlink(full_path);
    }
    if (recv_err) {
        /* The stream is mid-body, so this connection is unusable.  Answer and
           close rather than leaving a zombie socket holding lwIP pbufs. */
        jpp_http_resp_close_conn(conn);
        jpp_http_resp_send_err(conn, JPP_HTTP_400, "Receive error");
        return;
    }
    if (write_err) {
        jpp_http_resp_send_err(conn, JPP_HTTP_500, "Write error");
        return;
    }
    jpp_http_resp_set_status(conn, JPP_HTTP_201);
    jpp_http_resp_send(conn, NULL, 0);
}

static void handle_delete(jpp_http_conn_t *conn)
{
    char full_path[FULL_PATH_MAX];
    if (!build_path(jpp_http_uri(conn), full_path, sizeof(full_path))) {
        jpp_http_resp_send_err(conn, JPP_HTTP_400, "Bad path");
        return;
    }
    struct stat st;
    if (stat(full_path, &st) != 0) {
        jpp_http_resp_send_err(conn, JPP_HTTP_404, "Not found");
        return;
    }
    if (!delete_recursive(full_path)) {
        jpp_http_resp_send_err(conn, JPP_HTTP_500, "Delete failed");
        return;
    }
    jpp_http_resp_set_status(conn, JPP_HTTP_204);
    jpp_http_resp_send(conn, NULL, 0);
}

static void handle_move(jpp_http_conn_t *conn)
{
    char full_src[FULL_PATH_MAX];
    if (!build_path(jpp_http_uri(conn), full_src, sizeof(full_src))) {
        jpp_http_resp_send_err(conn, JPP_HTTP_400, "Bad source path");
        return;
    }

    const char *dest_uri = jpp_http_header(conn, "Destination");
    if (dest_uri == NULL) {
        jpp_http_resp_send_err(conn, JPP_HTTP_400, "Missing Destination");
        return;
    }
    /* Strip "http[s]://host" prefix to get bare URI path. */
    if (strncmp(dest_uri, "https://", 8) == 0) {
        dest_uri = strchr(dest_uri + 8, '/');
    } else if (strncmp(dest_uri, "http://", 7) == 0) {
        dest_uri = strchr(dest_uri + 7, '/');
    }
    if (dest_uri == NULL) {
        jpp_http_resp_send_err(conn, JPP_HTTP_400, "Bad Destination");
        return;
    }

    char full_dst[FULL_PATH_MAX];
    if (!build_path(dest_uri, full_dst, sizeof(full_dst))) {
        jpp_http_resp_send_err(conn, JPP_HTTP_400, "Bad destination path");
        return;
    }
    if (rename(full_src, full_dst) != 0) {
        jpp_http_resp_send_err(conn, JPP_HTTP_500, "Move failed");
        return;
    }
    jpp_http_resp_set_status(conn, JPP_HTTP_201);
    jpp_http_resp_send(conn, NULL, 0);
}

static void handle_mkcol(jpp_http_conn_t *conn)
{
    char full_path[FULL_PATH_MAX];
    if (!build_path(jpp_http_uri(conn), full_path, sizeof(full_path))) {
        jpp_http_resp_send_err(conn, JPP_HTTP_400, "Bad path");
        return;
    }
    if (mkdir(full_path, 0777) != 0) {
        jpp_http_resp_send_err(conn, JPP_HTTP_500, "Cannot create directory");
        return;
    }
    jpp_http_resp_set_status(conn, JPP_HTTP_201);
    jpp_http_resp_send(conn, NULL, 0);
}

/* ---- Dispatch ------------------------------------------------------------- */

static void webdav_dispatch(jpp_http_conn_t *conn, void *user_ctx)
{
    (void)user_ctx;
    const char *method = jpp_http_method(conn);

    /* OPTIONS stays unauthenticated: clients probe DAV support with it before
       they have any reason to send credentials. */
    if (strcmp(method, "OPTIONS") == 0) { handle_options(conn); return; }

    if (!check_auth(conn)) { deny_auth(conn); return; }

    if      (strcmp(method, "PROPFIND") == 0) { handle_propfind(conn); }
    else if (strcmp(method, "GET")      == 0) { handle_get(conn); }
    else if (strcmp(method, "HEAD")     == 0) { handle_get(conn); }
    else if (strcmp(method, "PUT")      == 0) { handle_put(conn); }
    else if (strcmp(method, "DELETE")   == 0) { handle_delete(conn); }
    else if (strcmp(method, "MOVE")     == 0) { handle_move(conn); }
    else if (strcmp(method, "MKCOL")    == 0) { handle_mkcol(conn); }
    else {
        jpp_http_resp_set_hdr(conn, "Allow",
            "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, MOVE, PROPFIND");
        jpp_http_resp_send_err(conn, JPP_HTTP_405, "Method not allowed");
    }
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

    jpp_http_server_config_t cfg = {
        .owner          = "webdav",
        .port           = s_fs.port,
        .stack_bytes    = WEBDAV_STACK_BYTES,
        .recv_timeout_s = 30u,
        .send_timeout_s = 10u,
        .handler        = webdav_dispatch,
        .user_ctx       = NULL,
    };

    jpp_http_result_t rc = jpp_http_server_start(&cfg);
    if (rc != JPP_HTTP_OK) {
        ESP_LOGE(TAG, "start failed: %s", jpp_http_result_name(rc));
        s_fs.state       = JPP_FILESERVER_STATE_ERROR;
        s_fs.password[0] = '\0';
        return JPP_FILESERVER_RESULT_START_FAILED;
    }

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
    jpp_http_result_t rc = jpp_http_server_stop();
    if (rc != JPP_HTTP_OK) {
        ESP_LOGE(TAG, "stop failed: %s", jpp_http_result_name(rc));
        return JPP_FILESERVER_RESULT_STOP_FAILED;
    }
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
