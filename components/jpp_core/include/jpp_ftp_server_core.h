#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * jpp_ftp_server_core — the FTP file server the File Server screen runs when
 * the protocol selector is set to FTP (the WebDAV choice runs on
 * jpp_http_server_core + jpp_fileserver_core instead).
 *
 * It is built the same way as jpp_http_server_core, for the same reason: every
 * byte it needs at runtime — the server task's stack and TCB, the control-line
 * buffer, and the large transfer buffer — is carved out of the shared
 * `jpp_app_pool` rather than the general heap, so a running transfer never
 * competes with the WiFi driver's frame buffers (see jpp_heap_monitor).  That
 * makes the FTP server a *foreground* activity: starting it acquires the pool,
 * so it is mutually exclusive with SD apps and with the HTTP servers by
 * construction, and stopping it hands the whole pool back.
 *
 * What it speaks (RFC 959 plus the extensions real clients rely on):
 *   - one user (`user`/`password` from the config); anonymous is refused
 *   - PASV / EPSV (what every client defaults to) and PORT / EPRT, the latter
 *     restricted to the control connection's own peer address (no FTP bounce)
 *   - LIST (unix `ls -l` shape), NLST, MLSD, MLST — FEAT advertises MLSD so
 *     FileZilla & co. use the machine-readable form
 *   - RETR / STOR / APPE streamed through the pool I/O buffer, REST for resume,
 *     SIZE, MDTM, DELE, MKD, RMD, RNFR/RNTO, CWD/CDUP/PWD, TYPE, NOOP, ABOR
 *   - AUTH TLS is answered 502 so clients that probe for FTPS fall back to
 *     plain FTP (there is no TLS on this hardware)
 *
 * Limits, all deliberate:
 *   - One control connection at a time (the transfer buffer is shared).  A
 *     second client is not left hanging in the backlog: while the server waits
 *     for a command it also polls the listener, and answers any extra
 *     connection with `421` and closes it, which every client understands as
 *     "try again later".  Set the client to a single connection.
 *   - One data connection per control connection, opened per transfer.
 *   - Binary transfers only: TYPE A is accepted but no line-ending conversion
 *     is done.
 *
 * Threading: the accept/serve loop runs in its own static task, stack included,
 * from the pool.  Nothing here touches the SSD1306 or the UI action queue.
 */

/* Fixed carves, all from the app pool. */
#define JPP_FTP_CTRL_BUF_BYTES   512u        /* one control line (+ pipelined tail) */
#define JPP_FTP_IO_BYTES_MIN    4096u        /* refuse to start below this           */
#define JPP_FTP_IO_BYTES_MAX   (32u * 1024u) /* diminishing returns above            */

#define JPP_FTP_USER_MAX   16u
#define JPP_FTP_PASS_MAX   32u
#define JPP_FTP_ROOT_MAX   64u
#define JPP_FTP_VPATH_MAX 256u   /* virtual ("/dir/file") path length      */

typedef enum {
    JPP_FTP_OK = 0,
    JPP_FTP_ERR_ARG,
    JPP_FTP_ERR_ALREADY_RUNNING,
    JPP_FTP_ERR_POOL_BUSY,    /* an app or an HTTP server holds the app pool */
    JPP_FTP_ERR_NO_MEMORY,    /* pool too small for the requested carve      */
    JPP_FTP_ERR_SOCKET,
    JPP_FTP_ERR_TASK,
    JPP_FTP_ERR_NOT_RUNNING,
    JPP_FTP_ERR_STOP_TIMEOUT,
} jpp_ftp_result_t;

typedef struct {
    const char *owner;           /* pool owner + log tag, e.g. "ftp"          */
    uint16_t    port;            /* control port, 21 on the device            */
    size_t      stack_bytes;     /* server task stack, from the pool           */
    const char *root;            /* VFS directory served as "/", e.g. "/sd"    */
    const char *user;
    const char *password;
    /* How long the control connection may sit with no command before it is
     * dropped with 421.  FTP clients keep the control connection open for
     * the whole session, and only one is served at a time, so this is what
     * bounds how long an abandoned client can lock everyone else out.
     * 0 → 120 s. */
    unsigned    idle_timeout_s;
    /* Mid-transfer stall budget on the data connection.  0 → 30 s. */
    unsigned    xfer_timeout_s;
} jpp_ftp_server_config_t;

/*
 * Acquire the app pool, carve the task/buffers out of it and start serving.
 * Returns JPP_FTP_ERR_POOL_BUSY if an app (or an HTTP server) holds the pool —
 * the caller should surface that rather than retry.
 */
jpp_ftp_result_t jpp_ftp_server_start(const jpp_ftp_server_config_t *config);

/*
 * Stop the server and release the app pool.  Blocks until the server task has
 * genuinely exited — it runs on pool memory, so the pool must not be handed to
 * anything else before then.  Returns JPP_FTP_ERR_STOP_TIMEOUT (and keeps the
 * pool) if the task refuses to exit, which leaves the server marked running.
 */
jpp_ftp_result_t jpp_ftp_server_stop(void);

bool        jpp_ftp_server_is_running(void);
const char *jpp_ftp_result_name(jpp_ftp_result_t result);

#ifdef __cplusplus
}
#endif
