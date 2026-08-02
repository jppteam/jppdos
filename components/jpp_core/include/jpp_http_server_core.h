#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * jpp_http_server_core — the minimal HTTP/1.1 server the WebDAV and LRV
 * verification screens run on.
 *
 * It exists instead of ESP-IDF's `esp_http_server` for one reason: every byte
 * it needs at runtime is carved out of the shared `jpp_app_pool` (64 KB static
 * .bss) rather than the general heap.  `httpd_start()` allocates its task
 * stack, its socket table and its scratch buffers with malloc and gives no way
 * to redirect them, so a running WebDAV transfer competed for heap with the
 * WiFi driver's management/data frames and lwIP's pbufs — the "network died
 * mid-transfer" failure mode (see jpp_heap_monitor).  Running out of the app
 * pool inverts that: the servers cost the heap nothing, and the memory is only
 * committed while a server is actually on screen.
 *
 * Consequences of that choice, all deliberate:
 *   - A server is a *foreground* activity.  Starting one acquires the app pool,
 *     so it is mutually exclusive with a running SD app (and with the other
 *     server) by construction, and stopping it returns the whole 64 KB.
 *   - Only one server instance exists at a time (single static control block).
 *   - One connection is served at a time — the same limit `max_open_sockets=1`
 *     imposed before — because the handler I/O buffer is shared.
 *   - The I/O buffer is whatever is left of the pool (capped at
 *     JPP_HTTP_IO_BYTES_MAX), i.e. 32 KB rather than the 4 KB static buffer it
 *     replaces, which is where the WebDAV throughput win comes from.
 *
 * Threading: the accept/serve loop runs in its own static task, stack included,
 * from the pool.  The handler callback runs on that task; it must not touch the
 * SSD1306 or the UI action queue.
 */

/*
 * Request headers retained per request.  Generous on purpose: real WebDAV
 * clients (Finder, the Windows redirector) send well over a dozen, and a
 * header pushed past this limit is invisible to jpp_http_header() — which for
 * Authorization or Destination means a working request failing for no visible
 * reason.  Overflow is logged rather than silent.
 */
#define JPP_HTTP_MAX_REQ_HEADERS 32u

/* Fixed carves, all from the app pool. */
#define JPP_HTTP_REQ_BUF_BYTES   4096u        /* request line + header block   */
#define JPP_HTTP_RESP_HDR_BYTES   512u        /* response header accumulator   */
#define JPP_HTTP_IO_BYTES_MIN    4096u        /* refuse to start below this    */
#define JPP_HTTP_IO_BYTES_MAX   (32u * 1024u) /* diminishing returns above     */

/* Status lines, spelled the way they go on the wire. */
#define JPP_HTTP_200 "200 OK"
#define JPP_HTTP_201 "201 Created"
#define JPP_HTTP_204 "204 No Content"
#define JPP_HTTP_207 "207 Multi-Status"
#define JPP_HTTP_400 "400 Bad Request"
#define JPP_HTTP_401 "401 Unauthorized"
#define JPP_HTTP_404 "404 Not Found"
#define JPP_HTTP_405 "405 Method Not Allowed"
#define JPP_HTTP_500 "500 Internal Server Error"

typedef enum {
    JPP_HTTP_OK = 0,
    JPP_HTTP_ERR_ARG,
    JPP_HTTP_ERR_ALREADY_RUNNING,
    JPP_HTTP_ERR_POOL_BUSY,   /* an app or the other server holds the app pool */
    JPP_HTTP_ERR_NO_MEMORY,   /* pool too small for the requested carve        */
    JPP_HTTP_ERR_SOCKET,
    JPP_HTTP_ERR_TASK,
    JPP_HTTP_ERR_NOT_RUNNING,
    JPP_HTTP_ERR_STOP_TIMEOUT,
} jpp_http_result_t;

/* Opaque per-connection state; valid only inside the handler callback. */
typedef struct jpp_http_conn jpp_http_conn_t;

/*
 * Request handler.  Called once per request with the request line and headers
 * already parsed.  It must produce exactly one response (any of the
 * jpp_http_resp_* terminators).  Leaving without responding, or returning with
 * an unread request body, closes the connection.
 */
typedef void (*jpp_http_handler_fn)(jpp_http_conn_t *conn, void *user_ctx);

typedef struct {
    const char         *owner;           /* pool owner + log tag, e.g. "webdav" */
    uint16_t            port;
    size_t              stack_bytes;     /* server task stack, from the pool    */
    /* Mid-message timeouts — how long the peer may stall part-way through a
     * request or response.  A connection sitting *between* requests is dropped
     * far sooner (5 s), since it occupies the one connection slot.
     */
    unsigned            recv_timeout_s;  /* 0 → 30                              */
    unsigned            send_timeout_s;  /* 0 → 10                              */
    jpp_http_handler_fn handler;
    void               *user_ctx;
} jpp_http_server_config_t;

/*
 * Acquire the app pool, carve the task/buffers out of it and start serving.
 * Returns JPP_HTTP_ERR_POOL_BUSY if an app (or the other server) holds the
 * pool — the caller should surface that rather than retry.
 */
jpp_http_result_t jpp_http_server_start(const jpp_http_server_config_t *config);

/*
 * Stop the server and release the app pool.  Blocks until the server task has
 * genuinely exited — it runs on pool memory, so the pool must not be handed to
 * anything else before then.  Returns JPP_HTTP_ERR_STOP_TIMEOUT (and keeps the
 * pool) if the task refuses to exit, which leaves the server marked running.
 */
jpp_http_result_t jpp_http_server_stop(void);

bool        jpp_http_server_is_running(void);
/* Owner label of the running server, or NULL when stopped. */
const char *jpp_http_server_owner(void);
const char *jpp_http_result_name(jpp_http_result_t result);

/* ---- Request accessors (handler context only) ---------------------------- */

/*
 * Method verb as sent, e.g. "GET", "PROPFIND". Never NULL.
 * HEAD needs no special handling in a handler: treat it as GET and the
 * response layer suppresses the body while keeping the headers intact.
 */
const char *jpp_http_method(const jpp_http_conn_t *conn);
/* Request target as sent (not percent-decoded, matching the previous server). */
const char *jpp_http_uri(const jpp_http_conn_t *conn);
/* Case-insensitive header lookup; NULL when absent. */
const char *jpp_http_header(const jpp_http_conn_t *conn, const char *name);
/* Declared body length (0 when there is no body). */
size_t      jpp_http_content_len(const jpp_http_conn_t *conn);

/*
 * Shared handler scratch buffer — the biggest single carve from the pool, and
 * the buffer request bodies and file contents should be streamed through.
 * Valid for the duration of the request.
 */
void *jpp_http_io_buf(jpp_http_conn_t *conn, size_t *out_size);

/*
 * Read up to len bytes of the request body.
 * Returns  >0 bytes read, 0 on a receive timeout (the peer is still connected —
 * the caller may retry), -1 on a closed or broken connection.
 */
int jpp_http_recv(jpp_http_conn_t *conn, void *buf, size_t len);

/*
 * Emit a bare "100 Continue" interim response.  Clients that send
 * "Expect: 100-continue" (macOS Finder does, on every PUT) withhold the body
 * until they see it.
 */
bool jpp_http_send_continue(jpp_http_conn_t *conn);

/* ---- Response builders --------------------------------------------------- */

/* Status line without the "HTTP/1.1 " prefix, e.g. JPP_HTTP_201. Default 200. */
void jpp_http_resp_set_status(jpp_http_conn_t *conn, const char *status);
/* Close the connection once this response completes (no keep-alive). */
void jpp_http_resp_close_conn(jpp_http_conn_t *conn);
void jpp_http_resp_set_type(jpp_http_conn_t *conn, const char *content_type);
/* Adds one response header. Both strings are copied. */
void jpp_http_resp_set_hdr(jpp_http_conn_t *conn, const char *name, const char *value);

/*
 * Send a complete fixed-length response (Content-Length set from len).  Pass
 * len < 0 for a NUL-terminated body.  Returns false if the peer went away.
 */
bool jpp_http_resp_send(jpp_http_conn_t *conn, const void *body, ssize_t len);

/*
 * Stream a body whose length is known up front: begin(content_len), then
 * write() the bytes, then finish().  Preferred over the chunked API for file
 * downloads — no per-chunk framing, one send() per buffer, and clients get a
 * real Content-Length (progress bars, and Finder's range logic).
 */
bool jpp_http_resp_begin(jpp_http_conn_t *conn, size_t content_len);
bool jpp_http_resp_write(jpp_http_conn_t *conn, const void *data, size_t len);
bool jpp_http_resp_finish(jpp_http_conn_t *conn);

/*
 * Stream a chunked response.  The first call emits the header block with
 * Transfer-Encoding: chunked; a NULL/zero-length chunk terminates the body.
 * Returns false once the peer has gone away — stop sending at that point, since
 * every further send waits out the send timeout before failing.
 */
bool jpp_http_resp_send_chunk(jpp_http_conn_t *conn, const void *data, ssize_t len);
bool jpp_http_resp_sendstr_chunk(jpp_http_conn_t *conn, const char *str);

/* Fixed-length text/plain error response. */
bool jpp_http_resp_send_err(jpp_http_conn_t *conn, const char *status, const char *msg);

#ifdef __cplusplus
}
#endif
