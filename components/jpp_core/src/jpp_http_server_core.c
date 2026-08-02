#include "../include/jpp_http_server_core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "jpp_app_pool.h"

static const char *TAG = "jpp_httpd";

/* How long a blocking wait sits in select() before re-checking stop_req.  The
   stop path also shuts the client socket down, so this only bounds the case
   where the socket is idle. */
#define POLL_SLICE_MS   200
/* Upper bound on how long jpp_http_server_stop() waits for the task to exit. */
#define STOP_WAIT_MS   3000
/* How long an established connection may sit between requests before it is
   dropped.  Short because it occupies the one connection slot. */
#define IDLE_TIMEOUT_MS 5000
#define TASK_PRIORITY     5u   /* what esp_http_server used: tskIDLE_PRIORITY+5 */

struct jpp_http_conn {
    int      sock;

    /* ---- request ---- */
    char    *req_buf;                    /* JPP_HTTP_REQ_BUF_BYTES            */
    char    *method;
    char    *uri;
    char    *version;
    struct {
        char *name;
        char *value;
    }        hdr[JPP_HTTP_MAX_REQ_HEADERS];
    size_t   hdr_count;
    size_t   content_len;
    size_t   body_read;                  /* body bytes handed to the handler  */
    char    *body_pre;                   /* body that arrived with the headers */
    size_t   body_pre_len;
    bool     keep_alive;

    /* ---- response ---- */
    char     status[40];
    char    *resp_hdr;                   /* JPP_HTTP_RESP_HDR_BYTES           */
    size_t   resp_hdr_len;
    bool     resp_started;
    bool     resp_chunked;
    bool     resp_done;
    bool     suppress_body;              /* HEAD: headers only                */
    bool     peer_gone;

    /* ---- handler scratch ---- */
    uint8_t *io;
    size_t   io_size;
};

static struct {
    bool                     running;
    volatile bool            stop_req;
    volatile bool            task_exited;
    TaskHandle_t             task;
    StaticTask_t            *tcb;
    int                      listen_sock;
    int                      client_sock;   /* guarded by sock_mtx */
    SemaphoreHandle_t        sock_mtx;
    jpp_http_handler_fn      handler;
    void                    *user_ctx;
    unsigned                 recv_timeout_s;
    unsigned                 send_timeout_s;
    jpp_http_conn_t         *conn;
    char                     owner[16];
} s_srv;

/* The only static RAM this component keeps: the mutex control block that
   guards the client socket fd against the stop path (see sock_close()). */
static StaticSemaphore_t s_sock_mtx_buf;

/* ---- Socket helpers ------------------------------------------------------ */

static void sock_lock(void)   { if (s_srv.sock_mtx) xSemaphoreTake(s_srv.sock_mtx, portMAX_DELAY); }
static void sock_unlock(void) { if (s_srv.sock_mtx) xSemaphoreGive(s_srv.sock_mtx); }

/* Publish the accepted fd so stop() can shut it down, taking the lock so it
   can never act on an fd that has already been closed and recycled. */
static void sock_publish(int fd)
{
    sock_lock();
    s_srv.client_sock = fd;
    sock_unlock();
}

static void sock_close_client(void)
{
    sock_lock();
    if (s_srv.client_sock >= 0) {
        close(s_srv.client_sock);
        s_srv.client_sock = -1;
    }
    sock_unlock();
}

/* 1 = readable, 0 = timed out, -1 = error. */
static int wait_readable(int sock, int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int r = select(sock + 1, &rfds, NULL, NULL, &tv);
    if (r > 0)  { return 1; }
    if (r == 0) { return 0; }
    return (errno == EINTR) ? 0 : -1;
}

/*
 * Read into buf, waiting up to budget_ms and giving up early if the server is
 * stopping.
 * Returns >0 bytes, 0 on timeout (peer still connected), -1 on close/error.
 */
static int sock_recv_to(jpp_http_conn_t *conn, void *buf, size_t len, int budget_ms)
{
    int waited_ms = 0;

    while (waited_ms < budget_ms) {
        if (s_srv.stop_req) { return -1; }
        int ready = wait_readable(conn->sock, POLL_SLICE_MS);
        if (ready < 0) { return -1; }
        if (ready == 0) { waited_ms += POLL_SLICE_MS; continue; }

        int n = recv(conn->sock, buf, len, 0);
        if (n > 0)  { return n; }
        if (n == 0) { return -1; }              /* orderly shutdown by peer */
        if (errno == EINTR) { continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            waited_ms += POLL_SLICE_MS;
            continue;
        }
        return -1;
    }
    return 0;
}

/* Mid-message read: the peer owes us bytes, so give it the full timeout. */
static int sock_recv(jpp_http_conn_t *conn, void *buf, size_t len)
{
    return sock_recv_to(conn, buf, len, (int)s_srv.recv_timeout_s * 1000);
}

static bool send_all(jpp_http_conn_t *conn, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len > 0u) {
        if (conn->peer_gone || s_srv.stop_req) { conn->peer_gone = true; return false; }
        int n = send(conn->sock, p, len, 0);
        if (n > 0) {
            p   += (size_t)n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) { continue; }
        /* EAGAIN here means SO_SNDTIMEO expired: the peer stopped reading.
           Treat it as gone rather than waiting again — every further send
           would burn another full timeout. */
        conn->peer_gone = true;
        return false;
    }
    return true;
}

/* ---- Request parsing ----------------------------------------------------- */

static char *skip_ows(char *p)
{
    while (*p == ' ' || *p == '\t') { p++; }
    return p;
}

/* Locate the CRLFCRLF terminating the header block. Returns its offset or -1. */
static int find_header_end(const char *buf, size_t len)
{
    if (len < 4u) { return -1; }
    for (size_t i = 0u; i + 3u < len; i++) {
        if (buf[i] == '\r' && buf[i + 1u] == '\n' &&
            buf[i + 2u] == '\r' && buf[i + 3u] == '\n') {
            return (int)i;
        }
    }
    return -1;
}

static void parse_header_block(jpp_http_conn_t *conn, char *block)
{
    /* First line: METHOD SP request-target SP HTTP-version */
    char *line = block;
    char *eol  = strstr(line, "\r\n");
    if (eol != NULL) { *eol = '\0'; }

    conn->method = line;
    char *sp = strchr(line, ' ');
    if (sp == NULL) { conn->uri = (char *)""; conn->version = (char *)""; return; }
    *sp = '\0';
    conn->uri = skip_ows(sp + 1);
    sp = strchr(conn->uri, ' ');
    if (sp != NULL) {
        *sp = '\0';
        conn->version = skip_ows(sp + 1);
    } else {
        conn->version = (char *)"HTTP/1.0";
    }

    /* HTTP/1.0 is close-by-default; 1.1 is keep-alive-by-default. */
    conn->keep_alive = (strcmp(conn->version, "HTTP/1.0") != 0);

    if (eol == NULL) { return; }
    line = eol + 2;

    while (*line != '\0') {
        eol = strstr(line, "\r\n");
        if (eol != NULL) { *eol = '\0'; }

        char *colon = strchr(line, ':');
        if (colon != NULL) {
            *colon = '\0';
            char *value = skip_ows(colon + 1);
            if (conn->hdr_count < JPP_HTTP_MAX_REQ_HEADERS) {
                conn->hdr[conn->hdr_count].name  = line;
                conn->hdr[conn->hdr_count].value = value;
                conn->hdr_count++;
            } else {
                /* Not fatal, but jpp_http_header() can no longer see it — say
                   so rather than let a request fail inexplicably. */
                ESP_LOGW(TAG, "%s: over %u request headers, dropped '%s'",
                         s_srv.owner, (unsigned)JPP_HTTP_MAX_REQ_HEADERS, line);
            }
            if (strcasecmp(line, "Content-Length") == 0) {
                long v = strtol(value, NULL, 10);
                conn->content_len = (v > 0) ? (size_t)v : 0u;
            } else if (strcasecmp(line, "Connection") == 0) {
                if (strcasecmp(value, "close") == 0) {
                    conn->keep_alive = false;
                } else if (strcasecmp(value, "keep-alive") == 0) {
                    conn->keep_alive = true;
                }
            }
        }
        if (eol == NULL) { break; }
        line = eol + 2;
    }
}

/*
 * Read and parse one request off the connection.
 * Returns 1 on success, 0 when the peer closed between requests, -1 on a
 * malformed or oversized request (the connection must then be closed).
 */
static int read_request(jpp_http_conn_t *conn)
{
    size_t used = 0u;
    int    hdr_end;

    conn->method       = (char *)"";
    conn->uri          = (char *)"";
    conn->version      = (char *)"";
    conn->hdr_count    = 0u;
    conn->content_len  = 0u;
    conn->body_read    = 0u;
    conn->body_pre     = NULL;
    conn->body_pre_len = 0u;
    conn->keep_alive   = true;

    for (;;) {
        hdr_end = find_header_end(conn->req_buf, used);
        if (hdr_end >= 0) { break; }
        if (used >= JPP_HTTP_REQ_BUF_BYTES) {
            ESP_LOGW(TAG, "%s: header block over %u bytes", s_srv.owner,
                     (unsigned)JPP_HTTP_REQ_BUF_BYTES);
            return -1;
        }
        /* Waiting for a request that has not started yet is an *idle* wait,
           and only one connection is served at a time — so hold the slot for
           IDLE_TIMEOUT_MS, not the full receive timeout, or a client that
           opens a keep-alive connection and sits on it (Finder does) locks
           every other client out for 30 seconds.  Once bytes have arrived the
           peer owes us the rest of the message and gets the full budget. */
        int n = (used == 0u)
                ? sock_recv_to(conn, conn->req_buf, JPP_HTTP_REQ_BUF_BYTES, IDLE_TIMEOUT_MS)
                : sock_recv(conn, conn->req_buf + used, JPP_HTTP_REQ_BUF_BYTES - used);
        if (n < 0) { return (used == 0u) ? 0 : -1; }  /* clean close between requests */
        if (n == 0) { return (used == 0u) ? 0 : -1; } /* idle timeout */
        used += (size_t)n;
    }

    conn->req_buf[hdr_end] = '\0';        /* terminate the header block */
    conn->body_pre     = conn->req_buf + hdr_end + 4;
    conn->body_pre_len = used - ((size_t)hdr_end + 4u);

    parse_header_block(conn, conn->req_buf);
    if (conn->body_pre_len > conn->content_len) {
        conn->body_pre_len = conn->content_len;  /* ignore pipelined extras */
    }
    return 1;
}

/* ---- Request accessors --------------------------------------------------- */

const char *jpp_http_method(const jpp_http_conn_t *conn)
{
    return (conn != NULL) ? conn->method : "";
}

const char *jpp_http_uri(const jpp_http_conn_t *conn)
{
    return (conn != NULL) ? conn->uri : "";
}

const char *jpp_http_header(const jpp_http_conn_t *conn, const char *name)
{
    if (conn == NULL || name == NULL) { return NULL; }
    for (size_t i = 0u; i < conn->hdr_count; i++) {
        if (strcasecmp(conn->hdr[i].name, name) == 0) {
            return conn->hdr[i].value;
        }
    }
    return NULL;
}

size_t jpp_http_content_len(const jpp_http_conn_t *conn)
{
    return (conn != NULL) ? conn->content_len : 0u;
}

void *jpp_http_io_buf(jpp_http_conn_t *conn, size_t *out_size)
{
    if (conn == NULL) { return NULL; }
    if (out_size != NULL) { *out_size = conn->io_size; }
    return conn->io;
}

int jpp_http_recv(jpp_http_conn_t *conn, void *buf, size_t len)
{
    if (conn == NULL || buf == NULL || len == 0u) { return -1; }

    size_t remaining = conn->content_len - conn->body_read;
    if (remaining == 0u) { return -1; }
    if (len > remaining) { len = remaining; }

    /* Bytes that arrived in the same TCP segment as the header block. */
    if (conn->body_pre_len > 0u) {
        size_t n = (len < conn->body_pre_len) ? len : conn->body_pre_len;
        memcpy(buf, conn->body_pre, n);
        conn->body_pre     += n;
        conn->body_pre_len -= n;
        conn->body_read    += n;
        return (int)n;
    }

    int n = sock_recv(conn, buf, len);
    if (n > 0) { conn->body_read += (size_t)n; }
    return n;
}

bool jpp_http_send_continue(jpp_http_conn_t *conn)
{
    static const char k100[] = "HTTP/1.1 100 Continue\r\n\r\n";
    if (conn == NULL) { return false; }
    return send_all(conn, k100, sizeof(k100) - 1u);
}

/* ---- Response ------------------------------------------------------------ */

static void reset_response(jpp_http_conn_t *conn)
{
    strcpy(conn->status, JPP_HTTP_200);
    conn->resp_hdr_len  = 0u;
    conn->resp_started  = false;
    conn->resp_chunked  = false;
    conn->resp_done     = false;
    conn->suppress_body = (strcmp(conn->method, "HEAD") == 0);
}

void jpp_http_resp_close_conn(jpp_http_conn_t *conn)
{
    if (conn != NULL) { conn->keep_alive = false; }
}

void jpp_http_resp_set_status(jpp_http_conn_t *conn, const char *status)
{
    if (conn == NULL || status == NULL) { return; }
    snprintf(conn->status, sizeof(conn->status), "%s", status);
}

void jpp_http_resp_set_hdr(jpp_http_conn_t *conn, const char *name, const char *value)
{
    if (conn == NULL || name == NULL || value == NULL || conn->resp_started) { return; }
    size_t room = JPP_HTTP_RESP_HDR_BYTES - conn->resp_hdr_len;
    int n = snprintf(conn->resp_hdr + conn->resp_hdr_len, room, "%s: %s\r\n", name, value);
    if (n < 0 || (size_t)n >= room) {
        ESP_LOGW(TAG, "%s: response headers full, dropped %s", s_srv.owner, name);
        conn->resp_hdr[conn->resp_hdr_len] = '\0';
        return;
    }
    conn->resp_hdr_len += (size_t)n;
}

void jpp_http_resp_set_type(jpp_http_conn_t *conn, const char *content_type)
{
    jpp_http_resp_set_hdr(conn, "Content-Type", content_type);
}

/* content_len < 0 selects Transfer-Encoding: chunked. */
static bool start_response(jpp_http_conn_t *conn, ssize_t content_len)
{
    char head[96];
    int  n;

    n = snprintf(head, sizeof(head), "HTTP/1.1 %s\r\n", conn->status);
    if (!send_all(conn, head, (size_t)n)) { return false; }
    if (conn->resp_hdr_len > 0u && !send_all(conn, conn->resp_hdr, conn->resp_hdr_len)) {
        return false;
    }
    if (content_len >= 0) {
        n = snprintf(head, sizeof(head), "Content-Length: %ld\r\n", (long)content_len);
    } else {
        n = snprintf(head, sizeof(head), "Transfer-Encoding: chunked\r\n");
        /* A HEAD response carries no body at all, so there is no chunked
           framing to delimit it — close instead of guessing. */
        if (conn->suppress_body) { conn->keep_alive = false; }
    }
    if (!send_all(conn, head, (size_t)n)) { return false; }

    n = snprintf(head, sizeof(head), "Connection: %s\r\n\r\n",
                 conn->keep_alive ? "keep-alive" : "close");
    if (!send_all(conn, head, (size_t)n)) { return false; }

    conn->resp_started = true;
    conn->resp_chunked = (content_len < 0);
    return true;
}

bool jpp_http_resp_send(jpp_http_conn_t *conn, const void *body, ssize_t len)
{
    if (conn == NULL || conn->resp_started) { return false; }
    size_t n = 0u;
    if (body != NULL) {
        n = (len < 0) ? strlen((const char *)body) : (size_t)len;
    }
    if (!start_response(conn, (ssize_t)n)) { conn->resp_done = true; return false; }
    bool ok = (n == 0u) || conn->suppress_body || send_all(conn, body, n);
    conn->resp_done = true;
    return ok;
}

bool jpp_http_resp_begin(jpp_http_conn_t *conn, size_t content_len)
{
    if (conn == NULL || conn->resp_started) { return false; }
    return start_response(conn, (ssize_t)content_len);
}

bool jpp_http_resp_write(jpp_http_conn_t *conn, const void *data, size_t len)
{
    if (conn == NULL || !conn->resp_started || conn->resp_chunked) { return false; }
    if (len == 0u || conn->suppress_body) { return true; }
    return send_all(conn, data, len);
}

bool jpp_http_resp_finish(jpp_http_conn_t *conn)
{
    if (conn == NULL) { return false; }
    conn->resp_done = true;
    return !conn->peer_gone;
}

bool jpp_http_resp_send_chunk(jpp_http_conn_t *conn, const void *data, ssize_t len)
{
    if (conn == NULL) { return false; }
    if (!conn->resp_started && !start_response(conn, -1)) {
        conn->resp_done = true;
        return false;
    }

    size_t n = 0u;
    if (data != NULL) {
        n = (len < 0) ? strlen((const char *)data) : (size_t)len;
    }
    if (n == 0u) {                      /* terminating chunk */
        bool ok = conn->suppress_body || send_all(conn, "0\r\n\r\n", 5u);
        conn->resp_done = true;
        return ok;
    }
    if (conn->suppress_body) { return true; }

    char size_line[16];
    int  sl = snprintf(size_line, sizeof(size_line), "%x\r\n", (unsigned)n);
    if (!send_all(conn, size_line, (size_t)sl)) { return false; }
    if (!send_all(conn, data, n))               { return false; }
    return send_all(conn, "\r\n", 2u);
}

bool jpp_http_resp_sendstr_chunk(jpp_http_conn_t *conn, const char *str)
{
    return jpp_http_resp_send_chunk(conn, str, -1);
}

bool jpp_http_resp_send_err(jpp_http_conn_t *conn, const char *status, const char *msg)
{
    if (conn == NULL) { return false; }
    if (conn->resp_started) { return false; }
    jpp_http_resp_set_status(conn, status);
    jpp_http_resp_set_type(conn, "text/plain");
    return jpp_http_resp_send(conn, msg, -1);
}

/* ---- Connection loop ----------------------------------------------------- */

static void serve_conn(jpp_http_conn_t *conn)
{
    for (;;) {
        if (s_srv.stop_req) { break; }

        int r = read_request(conn);
        if (r <= 0) { break; }

        conn->peer_gone = false;
        reset_response(conn);
        s_srv.handler(conn, s_srv.user_ctx);

        if (!conn->resp_done) {
            /* Client vanished mid-response — normal, nothing to say. */
            if (conn->peer_gone) { break; }
            /* Otherwise a handler returned without responding, which is a bug;
               answering keeps the client from hanging until its own timeout. */
            ESP_LOGE(TAG, "%s: handler sent no response for %s %s",
                     s_srv.owner, conn->method, conn->uri);
            if (!conn->resp_started) {
                conn->keep_alive = false;
                (void)jpp_http_resp_send_err(conn, JPP_HTTP_500, "No response");
            }
            break;
        }
        if (conn->peer_gone || !conn->keep_alive) { break; }
        /* An unread request body leaves the stream mid-message — the next
           request boundary is unknowable, so the connection has to go. */
        if (conn->body_read < conn->content_len) { break; }
    }
}

static void server_task(void *arg)
{
    (void)arg;
    jpp_http_conn_t *conn = s_srv.conn;

    while (!s_srv.stop_req) {
        int ready = wait_readable(s_srv.listen_sock, POLL_SLICE_MS);
        if (ready < 0) {
            ESP_LOGE(TAG, "%s: select on listener failed (errno %d)", s_srv.owner, errno);
            break;
        }
        if (ready == 0) { continue; }

        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof(peer);
        int cs = accept(s_srv.listen_sock, (struct sockaddr *)&peer, &peer_len);
        if (cs < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { continue; }
            ESP_LOGE(TAG, "%s: accept failed (errno %d)", s_srv.owner, errno);
            break;
        }

        struct timeval tv = { .tv_sec = (time_t)s_srv.recv_timeout_s, .tv_usec = 0 };
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        tv.tv_sec = (time_t)s_srv.send_timeout_s;
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sock_publish(cs);
        conn->sock      = cs;
        conn->peer_gone = false;
        serve_conn(conn);
        conn->sock = -1;
        sock_close_client();
    }

    /* Suspend rather than self-delete: this task's stack and TCB live in the
       app pool, so the stopper must be able to delete it at a known-safe point
       before handing the pool to anything else. */
    s_srv.task_exited = true;
    vTaskSuspend(NULL);
    for (;;) { vTaskDelay(portMAX_DELAY); }
}

/* ---- Lifecycle ----------------------------------------------------------- */

static int open_listener(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { return -1; }

    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(:%u) failed (errno %d)", (unsigned)port, errno);
        close(sock);
        return -1;
    }
    /* Backlog 2: one connection is served at a time (the handler I/O buffer is
       shared), the spare slot keeps a client's second connection from being
       refused outright while the first is in flight. */
    if (listen(sock, 2) < 0) {
        ESP_LOGE(TAG, "listen(:%u) failed (errno %d)", (unsigned)port, errno);
        close(sock);
        return -1;
    }
    return sock;
}

jpp_http_result_t jpp_http_server_start(const jpp_http_server_config_t *config)
{
    if (config == NULL || config->handler == NULL || config->port == 0u) {
        return JPP_HTTP_ERR_ARG;
    }
    if (s_srv.running) { return JPP_HTTP_ERR_ALREADY_RUNNING; }

    const char *owner = (config->owner != NULL) ? config->owner : "httpd";
    size_t stack_bytes = (config->stack_bytes != 0u) ? config->stack_bytes : 8192u;

    if (jpp_app_pool_acquire_as(owner, 0u, NULL) == NULL) {
        ESP_LOGW(TAG, "%s: app pool held by %s — cannot start", owner,
                 (jpp_app_pool_owner() != NULL) ? jpp_app_pool_owner() : "?");
        return JPP_HTTP_ERR_POOL_BUSY;
    }

    memset(&s_srv, 0, sizeof(s_srv));
    s_srv.listen_sock    = -1;
    s_srv.client_sock    = -1;
    s_srv.handler        = config->handler;
    s_srv.user_ctx       = config->user_ctx;
    s_srv.recv_timeout_s = (config->recv_timeout_s != 0u) ? config->recv_timeout_s : 30u;
    s_srv.send_timeout_s = (config->send_timeout_s != 0u) ? config->send_timeout_s : 10u;
    snprintf(s_srv.owner, sizeof(s_srv.owner), "%s", owner);

    /* Carve everything out of the pool, biggest-consumer last so the I/O
       buffer gets all the space the fixed allocations left behind. */
    s_srv.tcb              = jpp_app_pool_alloc(sizeof(StaticTask_t), 16u);
    StackType_t     *stack = jpp_app_pool_alloc(stack_bytes, 16u);
    jpp_http_conn_t *conn  = jpp_app_pool_alloc(sizeof(*conn), 16u);
    if (s_srv.tcb == NULL || stack == NULL || conn == NULL) {
        jpp_app_pool_release();
        return JPP_HTTP_ERR_NO_MEMORY;
    }
    memset(conn, 0, sizeof(*conn));
    conn->sock     = -1;
    conn->req_buf  = jpp_app_pool_alloc(JPP_HTTP_REQ_BUF_BYTES, 4u);
    conn->resp_hdr = jpp_app_pool_alloc(JPP_HTTP_RESP_HDR_BYTES, 4u);
    if (conn->req_buf == NULL || conn->resp_hdr == NULL) {
        jpp_app_pool_release();
        return JPP_HTTP_ERR_NO_MEMORY;
    }

    size_t io_size = jpp_app_pool_avail() & ~(size_t)0x1FF;  /* round down to 512 */
    if (io_size > JPP_HTTP_IO_BYTES_MAX) { io_size = JPP_HTTP_IO_BYTES_MAX; }
    if (io_size < JPP_HTTP_IO_BYTES_MIN) {
        ESP_LOGE(TAG, "%s: only %zu bytes left for the I/O buffer",
                 owner, jpp_app_pool_avail());
        jpp_app_pool_release();
        return JPP_HTTP_ERR_NO_MEMORY;
    }
    conn->io      = jpp_app_pool_alloc(io_size, 16u);
    conn->io_size = io_size;
    if (conn->io == NULL) {
        jpp_app_pool_release();
        return JPP_HTTP_ERR_NO_MEMORY;
    }
    s_srv.conn = conn;

    s_srv.listen_sock = open_listener(config->port);
    if (s_srv.listen_sock < 0) {
        jpp_app_pool_release();
        return JPP_HTTP_ERR_SOCKET;
    }

    s_srv.sock_mtx = xSemaphoreCreateMutexStatic(&s_sock_mtx_buf);
    s_srv.task = xTaskCreateStatic(server_task, s_srv.owner, stack_bytes, NULL,
                                   TASK_PRIORITY, stack, s_srv.tcb);
    if (s_srv.task == NULL) {
        close(s_srv.listen_sock);
        s_srv.listen_sock = -1;
        vSemaphoreDelete(s_srv.sock_mtx);
        s_srv.sock_mtx = NULL;
        jpp_app_pool_release();
        return JPP_HTTP_ERR_TASK;
    }

    s_srv.running = true;
    ESP_LOGI(TAG, "%s: listening on :%u (pool: %zu B stack, %zu B I/O, %zu B free)",
             s_srv.owner, (unsigned)config->port, stack_bytes, io_size,
             jpp_app_pool_avail());
    return JPP_HTTP_OK;
}

jpp_http_result_t jpp_http_server_stop(void)
{
    if (!s_srv.running) { return JPP_HTTP_ERR_NOT_RUNNING; }

    s_srv.stop_req = true;
    /* Unblock a connection parked in recv() or send(); the poll slices alone
       would only cover the idle case. */
    sock_lock();
    if (s_srv.client_sock >= 0) { shutdown(s_srv.client_sock, SHUT_RDWR); }
    sock_unlock();

    int waited_ms = 0;
    while (!s_srv.task_exited && waited_ms < STOP_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }
    if (!s_srv.task_exited) {
        /* The pool must stay held: the task is still running on it. */
        ESP_LOGE(TAG, "%s: server task did not exit in %d ms — pool still held",
                 s_srv.owner, STOP_WAIT_MS);
        return JPP_HTTP_ERR_STOP_TIMEOUT;
    }

    vTaskDelete(s_srv.task);
    s_srv.task = NULL;

    sock_close_client();
    if (s_srv.listen_sock >= 0) {
        close(s_srv.listen_sock);
        s_srv.listen_sock = -1;
    }
    vSemaphoreDelete(s_srv.sock_mtx);
    s_srv.sock_mtx = NULL;

    s_srv.conn    = NULL;
    s_srv.running = false;
    jpp_app_pool_release();
    ESP_LOGI(TAG, "%s: stopped, app pool released", s_srv.owner);
    return JPP_HTTP_OK;
}

bool jpp_http_server_is_running(void)
{
    return s_srv.running;
}

const char *jpp_http_server_owner(void)
{
    return s_srv.running ? s_srv.owner : NULL;
}

const char *jpp_http_result_name(jpp_http_result_t result)
{
    switch (result) {
    case JPP_HTTP_OK:                 return "OK";
    case JPP_HTTP_ERR_ARG:            return "ARG";
    case JPP_HTTP_ERR_ALREADY_RUNNING:return "ALREADY_RUNNING";
    case JPP_HTTP_ERR_POOL_BUSY:      return "POOL_BUSY";
    case JPP_HTTP_ERR_NO_MEMORY:      return "NO_MEMORY";
    case JPP_HTTP_ERR_SOCKET:         return "SOCKET";
    case JPP_HTTP_ERR_TASK:           return "TASK";
    case JPP_HTTP_ERR_NOT_RUNNING:    return "NOT_RUNNING";
    case JPP_HTTP_ERR_STOP_TIMEOUT:   return "STOP_TIMEOUT";
    default:                          return "UNKNOWN";
    }
}
