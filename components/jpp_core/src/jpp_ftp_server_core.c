#include "../include/jpp_ftp_server_core.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
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

static const char *TAG = "jpp_ftpd";

/* How long a blocking wait sits in select() before re-checking stop_req.  The
   stop path also shuts every socket down, so this only bounds the idle case. */
#define POLL_SLICE_MS      200
/* Upper bound on how long jpp_ftp_server_stop() waits for the task to exit. */
#define STOP_WAIT_MS      3000
/* How long the server waits for the client to show up on the data port
   (passive) or accept our connection (active) after a transfer command. */
#define DATA_OPEN_MS     10000
/* Slow a password-guessing client down without holding the task for long
   enough to trip the stop timeout. */
#define LOGIN_FAIL_DELAY_MS 1000
#define TASK_PRIORITY        5u   /* same as jpp_http_server_core */

#define FULL_PATH_MAX (JPP_FTP_ROOT_MAX + JPP_FTP_VPATH_MAX)

/* Six months, the cutoff `ls -l` uses between "MMM DD HH:MM" and "MMM DD YYYY". */
#define LIST_RECENT_S (180L * 24L * 3600L)

typedef struct {
    int      ctrl;                     /* control connection                  */
    int      data;                     /* open data connection, -1           */
    int      pasv;                     /* passive-mode listener, -1          */
    bool     port_set;                 /* active-mode target pending         */
    struct sockaddr_in port_addr;
    struct in_addr     peer_ip;        /* the control connection's peer      */

    char    *line;                     /* JPP_FTP_CTRL_BUF_BYTES             */
    size_t   pend_len;                 /* bytes buffered past the last line  */

    bool     user_ok;                  /* USER matched; waiting for PASS     */
    bool     logged_in;
    char     cwd[JPP_FTP_VPATH_MAX];   /* virtual, always starts with '/'    */
    char     rnfr[FULL_PATH_MAX];      /* RNFR source awaiting RNTO          */
    bool     rnfr_set;
    long     rest_offset;              /* REST for the next RETR/STOR        */

    uint8_t *io;                       /* transfer buffer                    */
    size_t   io_size;
} ftp_conn_t;

static struct {
    bool               running;
    volatile bool      stop_req;
    volatile bool      task_exited;
    TaskHandle_t       task;
    StaticTask_t      *tcb;
    int                listen_sock;
    SemaphoreHandle_t  sock_mtx;       /* guards the fds below against stop() */
    ftp_conn_t        *conn;
    unsigned           idle_timeout_s;
    unsigned           xfer_timeout_s;
    char               owner[16];
    char               root[JPP_FTP_ROOT_MAX];
    char               user[JPP_FTP_USER_MAX + 1u];
    char               password[JPP_FTP_PASS_MAX + 1u];
} s_srv;

/* The only static RAM this component keeps: the mutex control block that
   guards the connection fds against the stop path. */
static StaticSemaphore_t s_sock_mtx_buf;

/* ---- Socket helpers ------------------------------------------------------ */

static void sock_lock(void)   { if (s_srv.sock_mtx) xSemaphoreTake(s_srv.sock_mtx, portMAX_DELAY); }
static void sock_unlock(void) { if (s_srv.sock_mtx) xSemaphoreGive(s_srv.sock_mtx); }

/* Close one of the connection's fds under the lock, so stop() can never act
   on an fd that has already been closed and recycled. */
static void sock_close_slot(int *slot)
{
    sock_lock();
    if (*slot >= 0) {
        close(*slot);
        *slot = -1;
    }
    sock_unlock();
}

static void sock_set_slot(int *slot, int fd)
{
    sock_lock();
    *slot = fd;
    sock_unlock();
}

/* Wait until one of up to two sockets is readable.  Returns a bitmask
   (1 = first, 2 = second), 0 on timeout, -1 on error.  Pass -1 to skip. */
static int wait_readable2(int a, int b, int timeout_ms)
{
    fd_set rfds;
    int    maxfd = -1;
    FD_ZERO(&rfds);
    if (a >= 0) { FD_SET(a, &rfds); maxfd = a; }
    if (b >= 0) { FD_SET(b, &rfds); if (b > maxfd) { maxfd = b; } }
    if (maxfd < 0) { return -1; }
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (r < 0)  { return (errno == EINTR) ? 0 : -1; }
    if (r == 0) { return 0; }
    int mask = 0;
    if (a >= 0 && FD_ISSET(a, &rfds)) { mask |= 1; }
    if (b >= 0 && FD_ISSET(b, &rfds)) { mask |= 2; }
    return mask;
}

static int wait_readable(int sock, int timeout_ms)
{
    int r = wait_readable2(sock, -1, timeout_ms);
    return (r > 0) ? 1 : r;
}

static bool send_all(int sock, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0u) {
        if (s_srv.stop_req) { return false; }
        int n = send(sock, p, len, 0);
        if (n > 0) {
            p   += (size_t)n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) { continue; }
        /* EAGAIN here means SO_SNDTIMEO expired: the peer stopped reading. */
        return false;
    }
    return true;
}

static void set_sock_timeouts(int sock, unsigned recv_s, unsigned send_s)
{
    struct timeval tv = { .tv_sec = (time_t)recv_s, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = (time_t)send_s;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* ---- Control-connection replies ------------------------------------------ */

static bool reply(ftp_conn_t *c, const char *text)
{
    char   buf[JPP_FTP_VPATH_MAX + 64u];
    int    n = snprintf(buf, sizeof(buf), "%s\r\n", text);
    if (n < 0) { return false; }
    if ((size_t)n >= sizeof(buf)) { n = (int)sizeof(buf) - 1; }
    return send_all(c->ctrl, buf, (size_t)n);
}

static bool replyf(ftp_conn_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool replyf(ftp_conn_t *c, const char *fmt, ...)
{
    char    buf[JPP_FTP_VPATH_MAX + 64u];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) { return false; }
    return reply(c, buf);
}

/* ---- Control-line reading ------------------------------------------------ */

/* Drop telnet IAC sequences (clients send IAC IP / IAC DM ahead of ABOR).
   Only the IAC-prefixed pair goes: 0xFF never occurs in UTF-8, but 0xF0–0xF4
   do (they lead 4-byte sequences), so nothing else is filtered — file names
   must survive intact. */
static size_t strip_telnet(char *buf, size_t len)
{
    size_t out = 0u;
    for (size_t i = 0u; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch == 0xFFu) {              /* IAC: skip it and its command byte */
            i++;
            continue;
        }
        buf[out++] = (char)ch;
    }
    return out;
}

/*
 * Take one command line out of the pending buffer into `out` (CRLF or LF
 * stripped, NUL-terminated).  Returns true if a complete line was there.
 */
static bool pend_take_line(ftp_conn_t *c, char *out, size_t out_size)
{
    char *nl = memchr(c->line, '\n', c->pend_len);
    if (nl == NULL) { return false; }
    size_t line_len = (size_t)(nl - c->line);
    size_t copy     = line_len;
    if (copy > 0u && c->line[copy - 1u] == '\r') { copy--; }
    if (copy >= out_size) { copy = out_size - 1u; }
    memcpy(out, c->line, copy);
    out[copy] = '\0';

    size_t consumed = line_len + 1u;
    memmove(c->line, c->line + consumed, c->pend_len - consumed);
    c->pend_len -= consumed;
    return true;
}

/* Answer a second client while the one connection slot is busy, rather than
   leaving it silent in the backlog until its own connect timeout fires. */
static void reject_extra_client(void)
{
    struct sockaddr_in peer;
    socklen_t          peer_len = sizeof(peer);
    int cs = accept(s_srv.listen_sock, (struct sockaddr *)&peer, &peer_len);
    if (cs < 0) { return; }
    static const char k421[] =
        "421 Only one client at a time - try again later.\r\n";
    set_sock_timeouts(cs, 1u, 1u);
    (void)send_all(cs, k421, sizeof(k421) - 1u);
    close(cs);
    ESP_LOGW(TAG, "%s: refused a second client (one connection at a time)",
             s_srv.owner);
}

/*
 * Read one command line off the control connection.
 * Returns 1 on success, 0 when the peer closed or idled out, -1 on an
 * oversized line (the connection must then be closed).
 */
static int read_command(ftp_conn_t *c, char *out, size_t out_size)
{
    int waited_ms  = 0;
    int idle_ms    = (int)s_srv.idle_timeout_s * 1000;

    for (;;) {
        if (pend_take_line(c, out, out_size)) { return 1; }
        if (s_srv.stop_req) { return 0; }
        if (c->pend_len >= JPP_FTP_CTRL_BUF_BYTES) {
            ESP_LOGW(TAG, "%s: control line over %u bytes", s_srv.owner,
                     (unsigned)JPP_FTP_CTRL_BUF_BYTES);
            return -1;
        }
        if (waited_ms >= idle_ms) {
            (void)reply(c, "421 Idle timeout, closing control connection.");
            return 0;
        }

        int ready = wait_readable2(c->ctrl, s_srv.listen_sock, POLL_SLICE_MS);
        if (ready < 0) { return 0; }
        if (ready & 2) { reject_extra_client(); }
        if (!(ready & 1)) { waited_ms += POLL_SLICE_MS; continue; }

        int n = recv(c->ctrl, c->line + c->pend_len,
                     JPP_FTP_CTRL_BUF_BYTES - c->pend_len, 0);
        if (n == 0) { return 0; }                 /* orderly close by peer */
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                waited_ms += POLL_SLICE_MS;
                continue;
            }
            return 0;
        }
        size_t kept = strip_telnet(c->line + c->pend_len, (size_t)n);
        c->pend_len += kept;
        waited_ms = 0;                            /* activity resets idle */
    }
}

/*
 * Mid-transfer peek at the control connection: RFC 959 lets the client send
 * ABOR (usually preceded by telnet IAC IP / IAC DM) while a transfer is in
 * flight.  Anything else that arrives is kept for the command loop.
 */
static bool ctrl_abort_requested(ftp_conn_t *c)
{
    if (wait_readable(c->ctrl, 0) != 1) { return false; }
    if (c->pend_len >= JPP_FTP_CTRL_BUF_BYTES) { return false; }
    int n = recv(c->ctrl, c->line + c->pend_len,
                 JPP_FTP_CTRL_BUF_BYTES - c->pend_len, MSG_DONTWAIT);
    if (n <= 0) { return false; }
    c->pend_len += strip_telnet(c->line + c->pend_len, (size_t)n);

    /* Scan the buffered bytes for an ABOR line, case-insensitively. */
    for (size_t i = 0u; i + 4u <= c->pend_len; i++) {
        if (strncasecmp(c->line + i, "ABOR", 4u) == 0) {
            c->pend_len = 0u;      /* the abort supersedes anything queued */
            return true;
        }
    }
    return false;
}

/* ---- Paths --------------------------------------------------------------- */

/*
 * Resolve an FTP path argument against the connection's cwd into a
 * normalised virtual path: always starts with '/', never ends with one
 * except for the root itself, and `..` can never climb above the root — it
 * clamps there, which is what clients expect from CDUP at "/".
 */
static bool vpath_resolve(const ftp_conn_t *c, const char *arg,
                          char *out, size_t out_size)
{
    char joined[JPP_FTP_VPATH_MAX * 2u];
    int  n;
    if (arg == NULL || arg[0] == '\0') {
        n = snprintf(joined, sizeof(joined), "%s", c->cwd);
    } else if (arg[0] == '/') {
        n = snprintf(joined, sizeof(joined), "%s", arg);
    } else {
        n = snprintf(joined, sizeof(joined), "%s/%s", c->cwd, arg);
    }
    if (n < 0 || (size_t)n >= sizeof(joined)) { return false; }

    size_t out_len = 1u;
    out[0] = '/';
    out[1] = '\0';

    char *save = NULL;
    for (char *seg = strtok_r(joined, "/", &save); seg != NULL;
         seg = strtok_r(NULL, "/", &save)) {
        if (seg[0] == '\0' || strcmp(seg, ".") == 0) { continue; }
        if (strcmp(seg, "..") == 0) {
            if (out_len > 1u) {
                char *slash = strrchr(out, '/');
                out_len = (slash == out) ? 1u : (size_t)(slash - out);
                out[out_len] = '\0';
            }
            continue;
        }
        size_t seg_len = strlen(seg);
        size_t need    = out_len + (out_len > 1u ? 1u : 0u) + seg_len;
        if (need >= out_size) { return false; }
        if (out_len > 1u) { out[out_len++] = '/'; }
        memcpy(out + out_len, seg, seg_len);
        out_len += seg_len;
        out[out_len] = '\0';
    }
    return true;
}

static bool full_path(const char *vpath, char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "%s%s", s_srv.root,
                     (strcmp(vpath, "/") == 0) ? "" : vpath);
    return n > 0 && (size_t)n < out_size;
}

/* Resolve arg → virtual + full path in one go.  Sends 501 on a bad path. */
static bool resolve_arg(ftp_conn_t *c, const char *arg, char *vpath, char *full)
{
    if (!vpath_resolve(c, arg, vpath, JPP_FTP_VPATH_MAX) ||
        !full_path(vpath, full, FULL_PATH_MAX)) {
        (void)reply(c, "501 Path too long.");
        return false;
    }
    return true;
}

/* Strip `ls`-style option words ("LIST -la /dir") that some clients prepend. */
static const char *skip_list_options(const char *arg)
{
    while (arg != NULL && arg[0] == '-') {
        const char *sp = strchr(arg, ' ');
        if (sp == NULL) { return ""; }
        arg = sp + 1;
        while (*arg == ' ') { arg++; }
    }
    return (arg != NULL) ? arg : "";
}

/* ---- Listing formats ----------------------------------------------------- */

static void mtime_utc_stamp(time_t t, char *out, size_t out_size)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, out_size, "%Y%m%d%H%M%S", &tm);
}

/* `ls -l` line, the shape every LIST parser understands. */
static int format_list_line(char *buf, size_t cap, const char *name,
                            const struct stat *st, time_t now)
{
    struct tm tm;
    char      date[16];
    bool      is_dir = S_ISDIR(st->st_mode);

    localtime_r(&st->st_mtime, &tm);
    if (st->st_mtime <= now + 86400 && now - st->st_mtime < LIST_RECENT_S) {
        strftime(date, sizeof(date), "%b %e %H:%M", &tm);
    } else {
        strftime(date, sizeof(date), "%b %e  %Y", &tm);
    }
    return snprintf(buf, cap, "%crw%s 1 %s %s %10ld %s %s\r\n",
                    is_dir ? 'd' : '-',
                    is_dir ? "xrwxr-x" : "-rw-r--",
                    s_srv.user, s_srv.user,
                    is_dir ? 0L : (long)st->st_size, date, name);
}

/* RFC 3659 machine-readable fact line ("type=file;size=N;modify=...; name"). */
static int format_mlsx_line(char *buf, size_t cap, const char *name,
                            const struct stat *st, const char *prefix)
{
    char stamp[16];
    mtime_utc_stamp(st->st_mtime, stamp, sizeof(stamp));
    if (S_ISDIR(st->st_mode)) {
        return snprintf(buf, cap, "%stype=dir;modify=%s; %s\r\n", prefix, stamp, name);
    }
    return snprintf(buf, cap, "%stype=file;size=%ld;modify=%s; %s\r\n",
                    prefix, (long)st->st_size, stamp, name);
}

typedef enum { LIST_LS = 0, LIST_NAMES, LIST_MLSD } list_style_t;

/* ---- Data connection ----------------------------------------------------- */

static void data_close(ftp_conn_t *c)
{
    sock_close_slot(&c->data);
}

static void pasv_close(ftp_conn_t *c)
{
    sock_close_slot(&c->pasv);
}

/* Non-blocking connect with a bounded, stop-aware wait (active mode). */
static int active_connect(ftp_conn_t *c)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { return -1; }
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, (struct sockaddr *)&c->port_addr, sizeof(c->port_addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }
    int waited_ms = 0;
    bool connected = (rc == 0);
    while (!connected && waited_ms < DATA_OPEN_MS && !s_srv.stop_req) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = POLL_SLICE_MS * 1000 };
        int r = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (r < 0 && errno != EINTR) { break; }
        if (r > 0) {
            int       err = 0;
            socklen_t len = sizeof(err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) { connected = true; }
            break;
        }
        waited_ms += POLL_SLICE_MS;
    }
    if (!connected) {
        close(sock);
        return -1;
    }
    fcntl(sock, F_SETFL, flags);
    return sock;
}

/* Wait for the client on the passive listener. */
static int passive_accept(ftp_conn_t *c)
{
    int waited_ms = 0;
    while (waited_ms < DATA_OPEN_MS && !s_srv.stop_req) {
        int ready = wait_readable(c->pasv, POLL_SLICE_MS);
        if (ready < 0) { break; }
        if (ready == 0) { waited_ms += POLL_SLICE_MS; continue; }
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof(peer);
        int ds = accept(c->pasv, (struct sockaddr *)&peer, &peer_len);
        if (ds >= 0) { return ds; }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { break; }
    }
    return -1;
}

/*
 * Open the data connection for a transfer.  The 150 has already gone out;
 * on failure this answers 425 and returns false.  One data connection per
 * PASV/PORT: the passive listener is closed once its connection is taken.
 */
static bool data_open(ftp_conn_t *c)
{
    int ds = -1;
    if (c->pasv >= 0) {
        ds = passive_accept(c);
        pasv_close(c);
    } else if (c->port_set) {
        ds = active_connect(c);
        c->port_set = false;
    } else {
        (void)reply(c, "425 Use PASV or PORT first.");
        return false;
    }
    if (ds < 0) {
        (void)reply(c, "425 Can't open data connection.");
        return false;
    }
    set_sock_timeouts(ds, s_srv.xfer_timeout_s, s_srv.xfer_timeout_s);
    sock_set_slot(&c->data, ds);
    return true;
}

static bool data_ready(ftp_conn_t *c)
{
    return c->pasv >= 0 || c->port_set;
}

/* ---- Transfer commands --------------------------------------------------- */

/* Send a directory listing (or a single entry) over the data connection. */
static void cmd_list(ftp_conn_t *c, const char *arg, list_style_t style)
{
    char vpath[JPP_FTP_VPATH_MAX];
    char full[FULL_PATH_MAX];
    if (!resolve_arg(c, skip_list_options(arg), vpath, full)) { return; }

    struct stat st;
    if (stat(full, &st) != 0) {
        (void)reply(c, "550 No such file or directory.");
        return;
    }
    if (!data_ready(c)) {
        (void)reply(c, "425 Use PASV or PORT first.");
        return;
    }
    if (!reply(c, "150 Here comes the directory listing.")) { return; }
    if (!data_open(c)) { return; }

    time_t now  = time(NULL);
    bool   ok   = true;
    char   line[JPP_FTP_VPATH_MAX + 96u];

    if (!S_ISDIR(st.st_mode)) {
        const char *name = strrchr(vpath, '/');
        name = (name != NULL) ? name + 1 : vpath;
        int n = (style == LIST_MLSD)  ? format_mlsx_line(line, sizeof(line), name, &st, "")
              : (style == LIST_NAMES) ? snprintf(line, sizeof(line), "%s\r\n", name)
              :                         format_list_line(line, sizeof(line), name, &st, now);
        ok = (n > 0) && send_all(c->data, line, (size_t)n);
    } else {
        DIR *dir = opendir(full);
        if (dir == NULL) {
            data_close(c);
            (void)reply(c, "550 Can't open directory.");
            return;
        }
        struct dirent *ent;
        while (ok && (ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) { continue; }
            char child[FULL_PATH_MAX + 260u];
            snprintf(child, sizeof(child), "%s/%s", full, ent->d_name);
            struct stat cst;
            if (stat(child, &cst) != 0) { continue; }
            int n = (style == LIST_MLSD)  ? format_mlsx_line(line, sizeof(line), ent->d_name, &cst, "")
                  : (style == LIST_NAMES) ? snprintf(line, sizeof(line), "%s\r\n", ent->d_name)
                  :                         format_list_line(line, sizeof(line), ent->d_name, &cst, now);
            ok = (n > 0) && send_all(c->data, line, (size_t)n);
        }
        closedir(dir);
    }

    data_close(c);
    (void)reply(c, ok ? "226 Directory send OK."
                      : "426 Connection closed; transfer aborted.");
}

static void cmd_retr(ftp_conn_t *c, const char *arg)
{
    char vpath[JPP_FTP_VPATH_MAX];
    char full[FULL_PATH_MAX];
    long offset = c->rest_offset;
    c->rest_offset = 0;
    if (!resolve_arg(c, arg, vpath, full)) { return; }

    struct stat st;
    if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) {
        (void)reply(c, "550 Failed to open file.");
        return;
    }
    if (!data_ready(c)) {
        (void)reply(c, "425 Use PASV or PORT first.");
        return;
    }
    /* Raw open/read: the file streams straight off the SD card through the
       pool's transfer buffer, with no stdio layer to copy through. */
    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        (void)reply(c, "550 Failed to open file.");
        return;
    }
    if (offset > 0 && lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        close(fd);
        (void)reply(c, "554 Requested action not taken: invalid REST parameter.");
        return;
    }
    if (!replyf(c, "150 Opening BINARY mode data connection for %s (%ld bytes).",
                vpath, (long)st.st_size)) {
        close(fd);
        return;
    }
    if (!data_open(c)) {
        close(fd);
        return;
    }

    bool    ok      = true;
    bool    aborted = false;
    ssize_t got;
    while ((got = read(fd, c->io, c->io_size)) > 0) {
        if (ctrl_abort_requested(c)) { aborted = true; break; }
        if (!send_all(c->data, c->io, (size_t)got)) { ok = false; break; }
    }
    close(fd);
    data_close(c);

    if (aborted) {
        (void)reply(c, "426 Transfer aborted.");
        (void)reply(c, "226 Abort successful.");
    } else {
        (void)reply(c, ok ? "226 Transfer complete."
                          : "426 Connection closed; transfer aborted.");
    }
}

static void cmd_stor(ftp_conn_t *c, const char *arg, bool append)
{
    char vpath[JPP_FTP_VPATH_MAX];
    char full[FULL_PATH_MAX];
    long offset = c->rest_offset;
    c->rest_offset = 0;
    if (!resolve_arg(c, arg, vpath, full)) { return; }

    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        (void)reply(c, "553 Could not create file: is a directory.");
        return;
    }
    if (!data_ready(c)) {
        (void)reply(c, "425 Use PASV or PORT first.");
        return;
    }
    int flags = O_WRONLY | O_CREAT;
    if (append)            { flags |= O_APPEND; }
    else if (offset == 0)  { flags |= O_TRUNC; }
    int fd = open(full, flags, 0666);
    if (fd < 0) {
        (void)reply(c, "553 Could not create file.");
        return;
    }
    if (!append && offset > 0 && lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        close(fd);
        (void)reply(c, "554 Requested action not taken: invalid REST parameter.");
        return;
    }
    if (!reply(c, "150 Ok to send data.")) {
        close(fd);
        return;
    }
    if (!data_open(c)) {
        close(fd);
        return;
    }

    bool recv_err  = false;   /* socket-level failure: the partial file stays
                                 for a REST/APPE resume, like other servers   */
    bool write_err = false;   /* SD write failure: nothing to resume from     */
    bool aborted   = false;
    for (;;) {
        if (ctrl_abort_requested(c)) { aborted = true; break; }
        int n = recv(c->data, c->io, c->io_size, 0);
        if (n == 0) { break; }                      /* client closed = EOF */
        if (n < 0) {
            if (errno == EINTR) { continue; }
            recv_err = true;                        /* stall or reset       */
            break;
        }
        if (write(fd, c->io, (size_t)n) != (ssize_t)n) { write_err = true; break; }
    }
    close(fd);
    data_close(c);

    if (write_err) {
        unlink(full);
        (void)reply(c, "451 Requested action aborted: local error in processing.");
    } else if (aborted) {
        (void)reply(c, "426 Transfer aborted.");
        (void)reply(c, "226 Abort successful.");
    } else if (recv_err) {
        (void)reply(c, "426 Connection closed; transfer aborted.");
    } else {
        (void)reply(c, "226 Transfer complete.");
    }
}

/* ---- Passive / active mode setup ----------------------------------------- */

static bool cmd_pasv(ftp_conn_t *c, bool extended)
{
    pasv_close(c);
    c->port_set = false;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { return reply(c, "425 Can't open passive connection."); }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = 0,                    /* ephemeral */
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    socklen_t alen = sizeof(addr);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(sock, 1) < 0 ||
        getsockname(sock, (struct sockaddr *)&addr, &alen) < 0) {
        close(sock);
        return reply(c, "425 Can't open passive connection.");
    }
    sock_set_slot(&c->pasv, sock);
    unsigned port = ntohs(addr.sin_port);

    if (extended) {
        return replyf(c, "229 Entering Extended Passive Mode (|||%u|)", port);
    }
    /* The address to advertise is whichever of ours the client reached us
       on — read it off the control socket rather than asking the netif. */
    struct sockaddr_in local;
    socklen_t          llen = sizeof(local);
    if (getsockname(c->ctrl, (struct sockaddr *)&local, &llen) < 0) {
        pasv_close(c);
        return reply(c, "425 Can't open passive connection.");
    }
    uint32_t ip = ntohl(local.sin_addr.s_addr);
    return replyf(c, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
                  (unsigned)(ip >> 24) & 255u, (unsigned)(ip >> 16) & 255u,
                  (unsigned)(ip >> 8) & 255u,  (unsigned)ip & 255u,
                  port >> 8, port & 255u);
}

/* Accept an active-mode target only if it is the control peer itself, on an
   unprivileged port — the classic FTP-bounce guard. */
static bool set_active_target(ftp_conn_t *c, uint32_t ip_host_order, unsigned port)
{
    if (port < 1024u || port > 65535u ||
        htonl(ip_host_order) != c->peer_ip.s_addr) {
        return reply(c, "500 Illegal PORT command.");
    }
    pasv_close(c);
    memset(&c->port_addr, 0, sizeof(c->port_addr));
    c->port_addr.sin_family      = AF_INET;
    c->port_addr.sin_port        = htons((uint16_t)port);
    c->port_addr.sin_addr.s_addr = htonl(ip_host_order);
    c->port_set = true;
    return reply(c, "200 PORT command successful.");
}

static bool cmd_port(ftp_conn_t *c, const char *arg)
{
    unsigned h[4], p[2];
    if (arg == NULL ||
        sscanf(arg, "%u,%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3], &p[0], &p[1]) != 6 ||
        h[0] > 255u || h[1] > 255u || h[2] > 255u || h[3] > 255u ||
        p[0] > 255u || p[1] > 255u) {
        return reply(c, "501 Syntax error in PORT arguments.");
    }
    uint32_t ip = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                  ((uint32_t)h[2] << 8)  |  (uint32_t)h[3];
    return set_active_target(c, ip, (p[0] << 8) | p[1]);
}

/* EPRT |1|a.b.c.d|port| — only IPv4 exists on this device. */
static bool cmd_eprt(ftp_conn_t *c, const char *arg)
{
    if (arg == NULL || arg[0] == '\0') { return reply(c, "501 Syntax error in EPRT arguments."); }
    char     delim = arg[0];
    char     copy[64];
    snprintf(copy, sizeof(copy), "%s", arg + 1);
    char *proto = copy;
    char *host  = strchr(proto, delim);
    if (host == NULL) { return reply(c, "501 Syntax error in EPRT arguments."); }
    *host++ = '\0';
    char *port = strchr(host, delim);
    if (port == NULL) { return reply(c, "501 Syntax error in EPRT arguments."); }
    *port++ = '\0';
    char *end = strchr(port, delim);
    if (end != NULL) { *end = '\0'; }

    if (strcmp(proto, "1") != 0) {
        return reply(c, "522 Network protocol not supported, use (1)");
    }
    struct in_addr ia;
    if (inet_aton(host, &ia) == 0) {
        return reply(c, "501 Syntax error in EPRT arguments.");
    }
    return set_active_target(c, ntohl(ia.s_addr), (unsigned)strtoul(port, NULL, 10));
}

/* ---- Simple file-system commands ----------------------------------------- */

static bool cmd_cwd(ftp_conn_t *c, const char *arg)
{
    char vpath[JPP_FTP_VPATH_MAX];
    char full[FULL_PATH_MAX];
    if (!resolve_arg(c, arg, vpath, full)) { return true; }
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return reply(c, "550 Failed to change directory.");
    }
    snprintf(c->cwd, sizeof(c->cwd), "%s", vpath);
    return reply(c, "250 Directory successfully changed.");
}

/* 257 "<path>" — a quote inside the path is doubled, per RFC 959. */
static bool reply_path_257(ftp_conn_t *c, const char *vpath, const char *tail)
{
    char quoted[JPP_FTP_VPATH_MAX * 2u];
    size_t out = 0u;
    for (size_t i = 0u; vpath[i] != '\0' && out + 2u < sizeof(quoted); i++) {
        if (vpath[i] == '"') { quoted[out++] = '"'; }
        quoted[out++] = vpath[i];
    }
    quoted[out] = '\0';
    return replyf(c, "257 \"%s\" %s", quoted, tail);
}

static bool cmd_mkd(ftp_conn_t *c, const char *arg)
{
    char vpath[JPP_FTP_VPATH_MAX];
    char full[FULL_PATH_MAX];
    if (arg == NULL || arg[0] == '\0') { return reply(c, "501 Missing directory name."); }
    if (!resolve_arg(c, arg, vpath, full)) { return true; }
    if (mkdir(full, 0777) != 0) {
        return reply(c, "550 Create directory operation failed.");
    }
    return reply_path_257(c, vpath, "created");
}

static bool cmd_rmd(ftp_conn_t *c, const char *arg)
{
    char vpath[JPP_FTP_VPATH_MAX];
    char full[FULL_PATH_MAX];
    if (arg == NULL || arg[0] == '\0') { return reply(c, "501 Missing directory name."); }
    if (!resolve_arg(c, arg, vpath, full)) { return true; }
    if (rmdir(full) != 0) {
        return reply(c, "550 Remove directory operation failed.");
    }
    return reply(c, "250 Remove directory operation successful.");
}

static bool cmd_dele(ftp_conn_t *c, const char *arg)
{
    char vpath[JPP_FTP_VPATH_MAX];
    char full[FULL_PATH_MAX];
    if (arg == NULL || arg[0] == '\0') { return reply(c, "501 Missing file name."); }
    if (!resolve_arg(c, arg, vpath, full)) { return true; }
    struct stat st;
    if (stat(full, &st) != 0 || S_ISDIR(st.st_mode) || unlink(full) != 0) {
        return reply(c, "550 Delete operation failed.");
    }
    return reply(c, "250 Delete operation successful.");
}

static bool cmd_size(ftp_conn_t *c, const char *arg)
{
    char vpath[JPP_FTP_VPATH_MAX];
    char full[FULL_PATH_MAX];
    if (!resolve_arg(c, arg, vpath, full)) { return true; }
    struct stat st;
    if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) {
        return reply(c, "550 Could not get file size.");
    }
    return replyf(c, "213 %ld", (long)st.st_size);
}

static bool cmd_mdtm(ftp_conn_t *c, const char *arg)
{
    char vpath[JPP_FTP_VPATH_MAX];
    char full[FULL_PATH_MAX];
    if (!resolve_arg(c, arg, vpath, full)) { return true; }
    struct stat st;
    if (stat(full, &st) != 0) {
        return reply(c, "550 Could not get file modification time.");
    }
    char stamp[16];
    mtime_utc_stamp(st.st_mtime, stamp, sizeof(stamp));
    return replyf(c, "213 %s", stamp);
}

static bool cmd_mlst(ftp_conn_t *c, const char *arg)
{
    char vpath[JPP_FTP_VPATH_MAX];
    char full[FULL_PATH_MAX];
    if (!resolve_arg(c, arg, vpath, full)) { return true; }
    struct stat st;
    if (stat(full, &st) != 0) {
        return reply(c, "550 No such file or directory.");
    }
    char line[JPP_FTP_VPATH_MAX + 96u];
    /* MLST facts name the full path, not the leaf, and the line is indented
       by one space because it sits inside a multi-line 250 reply. */
    int n = format_mlsx_line(line, sizeof(line), vpath, &st, " ");
    if (n <= 0) { return reply(c, "550 No such file or directory."); }
    if (n >= 2 && line[n - 2] == '\r') { line[n - 2] = '\0'; }
    return reply(c, "250-Listing") && reply(c, line) && reply(c, "250 End");
}

static bool cmd_rnfr(ftp_conn_t *c, const char *arg)
{
    char vpath[JPP_FTP_VPATH_MAX];
    char full[FULL_PATH_MAX];
    c->rnfr_set = false;
    if (arg == NULL || arg[0] == '\0') { return reply(c, "501 Missing file name."); }
    if (!resolve_arg(c, arg, vpath, full)) { return true; }
    struct stat st;
    if (stat(full, &st) != 0) {
        return reply(c, "550 RNFR command failed.");
    }
    snprintf(c->rnfr, sizeof(c->rnfr), "%s", full);
    c->rnfr_set = true;
    return reply(c, "350 Ready for RNTO.");
}

static bool cmd_rnto(ftp_conn_t *c, const char *arg)
{
    char vpath[JPP_FTP_VPATH_MAX];
    char full[FULL_PATH_MAX];
    if (!c->rnfr_set) { return reply(c, "503 RNFR required first."); }
    c->rnfr_set = false;
    if (arg == NULL || arg[0] == '\0') { return reply(c, "501 Missing file name."); }
    if (!resolve_arg(c, arg, vpath, full)) { return true; }
    if (rename(c->rnfr, full) != 0) {
        return reply(c, "550 Rename failed.");
    }
    return reply(c, "250 Rename successful.");
}

/* ---- Dispatch ------------------------------------------------------------ */

static bool cmd_feat(ftp_conn_t *c)
{
    static const char feat[] =
        "211-Features:\r\n"
        " UTF8\r\n"
        " SIZE\r\n"
        " MDTM\r\n"
        " REST STREAM\r\n"
        " MLST type*;size*;modify*;\r\n"
        " MLSD\r\n"
        " PASV\r\n"
        " EPSV\r\n"
        " EPRT\r\n"
        "211 End\r\n";
    return send_all(c->ctrl, feat, sizeof(feat) - 1u);
}

/* Split "VERB arg" and upper-case the verb in place. */
static void split_command(char *line, char **verb, char **arg)
{
    while (*line == ' ') { line++; }
    *verb = line;
    char *sp = strchr(line, ' ');
    if (sp != NULL) {
        *sp = '\0';
        sp++;
        while (*sp == ' ') { sp++; }
        *arg = sp;
    } else {
        *arg = line + strlen(line);
    }
    for (char *p = *verb; *p != '\0'; p++) { *p = (char)toupper((unsigned char)*p); }
}

/* Returns false when the session should end. */
static bool dispatch(ftp_conn_t *c, char *line)
{
    char *verb;
    char *arg;
    split_command(line, &verb, &arg);

    /* Log the command with the argument, except PASS. */
    if (strcmp(verb, "PASS") == 0) {
        ESP_LOGD(TAG, "%s: < PASS ****", s_srv.owner);
    } else {
        ESP_LOGD(TAG, "%s: < %s %s", s_srv.owner, verb, arg);
    }

    /* ---- Always available ---- */
    if (strcmp(verb, "QUIT") == 0) {
        (void)reply(c, "221 Goodbye.");
        return false;
    }
    if (strcmp(verb, "NOOP") == 0) { return reply(c, "200 NOOP ok."); }
    if (strcmp(verb, "SYST") == 0) { return reply(c, "215 UNIX Type: L8"); }
    if (strcmp(verb, "FEAT") == 0) { return cmd_feat(c); }
    if (strcmp(verb, "HELP") == 0) { return reply(c, "214 JPPDOS FTP server."); }
    if (strcmp(verb, "OPTS") == 0) {
        if (strncasecmp(arg, "UTF8", 4u) == 0) { return reply(c, "200 UTF8 mode enabled."); }
        return reply(c, "501 Option not understood.");
    }
    if (strcmp(verb, "AUTH") == 0 || strcmp(verb, "PBSZ") == 0 || strcmp(verb, "PROT") == 0) {
        /* No TLS on this hardware: clients probing for FTPS fall back. */
        return reply(c, "502 Command not implemented.");
    }
    if (strcmp(verb, "USER") == 0) {
        c->logged_in = false;
        c->user_ok   = (strcmp(arg, s_srv.user) == 0);
        return reply(c, "331 Please specify the password.");
    }
    if (strcmp(verb, "PASS") == 0) {
        if (c->user_ok && strcmp(arg, s_srv.password) == 0) {
            c->logged_in = true;
            ESP_LOGI(TAG, "%s: user %s logged in", s_srv.owner, s_srv.user);
            return reply(c, "230 Login successful.");
        }
        c->logged_in = false;
        ESP_LOGW(TAG, "%s: login failed", s_srv.owner);
        vTaskDelay(pdMS_TO_TICKS(LOGIN_FAIL_DELAY_MS));
        return reply(c, "530 Login incorrect.");
    }
    if (!c->logged_in) {
        return reply(c, "530 Please login with USER and PASS.");
    }

    /* ---- Session parameters ---- */
    if (strcmp(verb, "TYPE") == 0) {
        char t = (char)toupper((unsigned char)arg[0]);
        if (t == 'I') { return reply(c, "200 Switching to Binary mode."); }
        if (t == 'A') { return reply(c, "200 Switching to ASCII mode."); }
        if (t == 'L') { return reply(c, "200 Switching to Binary mode."); }
        return reply(c, "504 Command not implemented for that parameter.");
    }
    if (strcmp(verb, "MODE") == 0) {
        return reply(c, toupper((unsigned char)arg[0]) == 'S'
                        ? "200 Mode set to S." : "504 Bad MODE command.");
    }
    if (strcmp(verb, "STRU") == 0) {
        return reply(c, toupper((unsigned char)arg[0]) == 'F'
                        ? "200 Structure set to F." : "504 Bad STRU command.");
    }
    if (strcmp(verb, "ALLO") == 0) { return reply(c, "202 ALLO command ignored."); }
    if (strcmp(verb, "ACCT") == 0) { return reply(c, "202 ACCT command ignored."); }
    if (strcmp(verb, "STAT") == 0) { return reply(c, "211 JPPDOS FTP server."); }
    if (strcmp(verb, "REST") == 0) {
        char *end   = NULL;
        long  value = strtol(arg, &end, 10);
        if (end == arg || value < 0) { return reply(c, "501 Bad REST offset."); }
        c->rest_offset = value;
        return replyf(c, "350 Restart position accepted (%ld).", value);
    }
    if (strcmp(verb, "ABOR") == 0) {
        /* Outside a transfer there is nothing to abort; mid-transfer ABOR is
           picked up by ctrl_abort_requested() instead of reaching here. */
        data_close(c);
        return reply(c, "226 No transfer to ABOR.");
    }

    /* ---- Data connection setup ---- */
    if (strcmp(verb, "PASV") == 0) { return cmd_pasv(c, false); }
    if (strcmp(verb, "EPSV") == 0) {
        if (strcasecmp(arg, "ALL") == 0) { return reply(c, "200 EPSV ALL ok."); }
        return cmd_pasv(c, true);
    }
    if (strcmp(verb, "PORT") == 0) { return cmd_port(c, arg); }
    if (strcmp(verb, "EPRT") == 0) { return cmd_eprt(c, arg); }

    /* ---- Navigation ---- */
    if (strcmp(verb, "PWD") == 0 || strcmp(verb, "XPWD") == 0) {
        return reply_path_257(c, c->cwd, "is the current directory");
    }
    if (strcmp(verb, "CWD") == 0 || strcmp(verb, "XCWD") == 0) { return cmd_cwd(c, arg); }
    if (strcmp(verb, "CDUP") == 0 || strcmp(verb, "XCUP") == 0) { return cmd_cwd(c, ".."); }

    /* ---- Listings ---- */
    if (strcmp(verb, "LIST") == 0) { cmd_list(c, arg, LIST_LS);    return true; }
    if (strcmp(verb, "NLST") == 0) { cmd_list(c, arg, LIST_NAMES); return true; }
    if (strcmp(verb, "MLSD") == 0) { cmd_list(c, arg, LIST_MLSD);  return true; }
    if (strcmp(verb, "MLST") == 0) { return cmd_mlst(c, arg); }

    /* ---- Transfers ---- */
    if (strcmp(verb, "RETR") == 0) { cmd_retr(c, arg);        return true; }
    if (strcmp(verb, "STOR") == 0) { cmd_stor(c, arg, false); return true; }
    if (strcmp(verb, "APPE") == 0) { cmd_stor(c, arg, true);  return true; }

    /* ---- File management ---- */
    if (strcmp(verb, "SIZE") == 0) { return cmd_size(c, arg); }
    if (strcmp(verb, "MDTM") == 0) { return cmd_mdtm(c, arg); }
    if (strcmp(verb, "DELE") == 0) { return cmd_dele(c, arg); }
    if (strcmp(verb, "MKD") == 0 || strcmp(verb, "XMKD") == 0) { return cmd_mkd(c, arg); }
    if (strcmp(verb, "RMD") == 0 || strcmp(verb, "XRMD") == 0) { return cmd_rmd(c, arg); }
    if (strcmp(verb, "RNFR") == 0) { return cmd_rnfr(c, arg); }
    if (strcmp(verb, "RNTO") == 0) { return cmd_rnto(c, arg); }

    if (strcmp(verb, "SITE") == 0 || strcmp(verb, "REIN") == 0 ||
        strcmp(verb, "SMNT") == 0 || strcmp(verb, "STOU") == 0) {
        return reply(c, "502 Command not implemented.");
    }
    return reply(c, "500 Unknown command.");
}

/* ---- Connection loop ----------------------------------------------------- */

static void conn_reset(ftp_conn_t *c)
{
    c->pend_len    = 0u;
    c->user_ok     = false;
    c->logged_in   = false;
    c->rnfr_set    = false;
    c->port_set    = false;
    c->rest_offset = 0;
    strcpy(c->cwd, "/");
}

static void serve_conn(ftp_conn_t *c)
{
    char cmd[JPP_FTP_CTRL_BUF_BYTES];

    conn_reset(c);
    if (!reply(c, "220 JPPDOS FTP server ready.")) { return; }

    while (!s_srv.stop_req) {
        int r = read_command(c, cmd, sizeof(cmd));
        if (r < 0) {
            (void)reply(c, "500 Line too long.");
            break;
        }
        if (r == 0) { break; }
        if (!dispatch(c, cmd)) { break; }
    }
    data_close(c);
    pasv_close(c);
}

static void server_task(void *arg)
{
    (void)arg;
    ftp_conn_t *conn = s_srv.conn;

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
        /* Replies are short; a peer that stops reading them is gone. */
        set_sock_timeouts(cs, s_srv.xfer_timeout_s, 10u);
        conn->peer_ip = peer.sin_addr;
        sock_set_slot(&conn->ctrl, cs);
        ESP_LOGI(TAG, "%s: client connected", s_srv.owner);
        serve_conn(conn);
        sock_close_slot(&conn->ctrl);
        ESP_LOGI(TAG, "%s: client disconnected", s_srv.owner);
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
    /* Backlog 2: extra clients are accepted and turned away with 421 by the
       command loop, so they only ever wait here during a transfer. */
    if (listen(sock, 2) < 0) {
        ESP_LOGE(TAG, "listen(:%u) failed (errno %d)", (unsigned)port, errno);
        close(sock);
        return -1;
    }
    return sock;
}

jpp_ftp_result_t jpp_ftp_server_start(const jpp_ftp_server_config_t *config)
{
    if (config == NULL || config->port == 0u || config->root == NULL ||
        config->user == NULL || config->password == NULL ||
        strlen(config->root) >= JPP_FTP_ROOT_MAX ||
        strlen(config->user) > JPP_FTP_USER_MAX ||
        strlen(config->password) > JPP_FTP_PASS_MAX) {
        return JPP_FTP_ERR_ARG;
    }
    if (s_srv.running) { return JPP_FTP_ERR_ALREADY_RUNNING; }

    const char *owner = (config->owner != NULL) ? config->owner : "ftpd";
    size_t stack_bytes = (config->stack_bytes != 0u) ? config->stack_bytes : 8192u;

    if (jpp_app_pool_acquire_as(owner, 0u, NULL) == NULL) {
        ESP_LOGW(TAG, "%s: app pool held by %s — cannot start", owner,
                 (jpp_app_pool_owner() != NULL) ? jpp_app_pool_owner() : "?");
        return JPP_FTP_ERR_POOL_BUSY;
    }

    memset(&s_srv, 0, sizeof(s_srv));
    s_srv.listen_sock    = -1;
    s_srv.idle_timeout_s = (config->idle_timeout_s != 0u) ? config->idle_timeout_s : 120u;
    s_srv.xfer_timeout_s = (config->xfer_timeout_s != 0u) ? config->xfer_timeout_s : 30u;
    snprintf(s_srv.owner, sizeof(s_srv.owner), "%s", owner);
    snprintf(s_srv.root, sizeof(s_srv.root), "%s", config->root);
    snprintf(s_srv.user, sizeof(s_srv.user), "%s", config->user);
    snprintf(s_srv.password, sizeof(s_srv.password), "%s", config->password);
    /* A trailing slash on the root would double up in full_path(). */
    size_t rlen = strlen(s_srv.root);
    if (rlen > 1u && s_srv.root[rlen - 1u] == '/') { s_srv.root[rlen - 1u] = '\0'; }

    /* Carve everything out of the pool, biggest-consumer last so the transfer
       buffer gets all the space the fixed allocations left behind. */
    s_srv.tcb          = jpp_app_pool_alloc(sizeof(StaticTask_t), 16u);
    StackType_t *stack = jpp_app_pool_alloc(stack_bytes, 16u);
    ftp_conn_t  *conn  = jpp_app_pool_alloc(sizeof(*conn), 16u);
    if (s_srv.tcb == NULL || stack == NULL || conn == NULL) {
        jpp_app_pool_release();
        return JPP_FTP_ERR_NO_MEMORY;
    }
    memset(conn, 0, sizeof(*conn));
    conn->ctrl = -1;
    conn->data = -1;
    conn->pasv = -1;
    conn->line = jpp_app_pool_alloc(JPP_FTP_CTRL_BUF_BYTES, 4u);
    if (conn->line == NULL) {
        jpp_app_pool_release();
        return JPP_FTP_ERR_NO_MEMORY;
    }

    size_t io_size = jpp_app_pool_avail() & ~(size_t)0x1FF;  /* round down to 512 */
    if (io_size > JPP_FTP_IO_BYTES_MAX) { io_size = JPP_FTP_IO_BYTES_MAX; }
    if (io_size < JPP_FTP_IO_BYTES_MIN) {
        ESP_LOGE(TAG, "%s: only %zu bytes left for the transfer buffer",
                 owner, jpp_app_pool_avail());
        jpp_app_pool_release();
        return JPP_FTP_ERR_NO_MEMORY;
    }
    conn->io      = jpp_app_pool_alloc(io_size, 16u);
    conn->io_size = io_size;
    if (conn->io == NULL) {
        jpp_app_pool_release();
        return JPP_FTP_ERR_NO_MEMORY;
    }
    s_srv.conn = conn;

    s_srv.listen_sock = open_listener(config->port);
    if (s_srv.listen_sock < 0) {
        jpp_app_pool_release();
        return JPP_FTP_ERR_SOCKET;
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
        return JPP_FTP_ERR_TASK;
    }

    s_srv.running = true;
    ESP_LOGI(TAG, "%s: listening on :%u (pool: %zu B stack, %zu B I/O, %zu B free)",
             s_srv.owner, (unsigned)config->port, stack_bytes, io_size,
             jpp_app_pool_avail());
    return JPP_FTP_OK;
}

jpp_ftp_result_t jpp_ftp_server_stop(void)
{
    if (!s_srv.running) { return JPP_FTP_ERR_NOT_RUNNING; }

    s_srv.stop_req = true;
    /* Unblock whatever the task is parked in: a command wait, a passive
       accept, or a data send/recv.  The poll slices alone only cover idle. */
    sock_lock();
    if (s_srv.conn->ctrl >= 0) { shutdown(s_srv.conn->ctrl, SHUT_RDWR); }
    if (s_srv.conn->data >= 0) { shutdown(s_srv.conn->data, SHUT_RDWR); }
    if (s_srv.conn->pasv >= 0) { shutdown(s_srv.conn->pasv, SHUT_RDWR); }
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
        return JPP_FTP_ERR_STOP_TIMEOUT;
    }

    vTaskDelete(s_srv.task);
    s_srv.task = NULL;

    sock_close_slot(&s_srv.conn->ctrl);
    sock_close_slot(&s_srv.conn->data);
    sock_close_slot(&s_srv.conn->pasv);
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
    return JPP_FTP_OK;
}

bool jpp_ftp_server_is_running(void)
{
    return s_srv.running;
}

const char *jpp_ftp_result_name(jpp_ftp_result_t result)
{
    switch (result) {
    case JPP_FTP_OK:                  return "OK";
    case JPP_FTP_ERR_ARG:             return "ARG";
    case JPP_FTP_ERR_ALREADY_RUNNING: return "ALREADY_RUNNING";
    case JPP_FTP_ERR_POOL_BUSY:       return "POOL_BUSY";
    case JPP_FTP_ERR_NO_MEMORY:       return "NO_MEMORY";
    case JPP_FTP_ERR_SOCKET:          return "SOCKET";
    case JPP_FTP_ERR_TASK:            return "TASK";
    case JPP_FTP_ERR_NOT_RUNNING:     return "NOT_RUNNING";
    case JPP_FTP_ERR_STOP_TIMEOUT:    return "STOP_TIMEOUT";
    default:                          return "UNKNOWN";
    }
}
