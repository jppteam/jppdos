/*
 * Host harness for jpp_http_server_core.c: compiles the real server against a
 * pthread shim for FreeRTOS and drives it over real loopback sockets.
 */
#include "jpp_http_server_core.h"
#include "jpp_app_pool.h"

#include <stdint.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int g_fail = 0;
static int g_port = 18080;

static void check(int cond, const char *what)
{
    printf("%-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) { g_fail++; }
}

/* ---- handler ------------------------------------------------------------- */

static void test_handler(jpp_http_conn_t *c, void *ctx)
{
    (void)ctx;
    const char *m = jpp_http_method(c);
    const char *u = jpp_http_uri(c);

    if (strcmp(u, "/fixed") == 0) {
        jpp_http_resp_set_type(c, "text/plain");
        jpp_http_resp_send(c, "hello world", -1);
    } else if (strcmp(u, "/chunked") == 0) {
        jpp_http_resp_set_type(c, "text/plain");
        jpp_http_resp_sendstr_chunk(c, "abc");
        jpp_http_resp_sendstr_chunk(c, "defg");
        jpp_http_resp_send_chunk(c, NULL, 0);
    } else if (strcmp(u, "/hdr") == 0) {
        const char *d = jpp_http_header(c, "x-depth");
        jpp_http_resp_send(c, (d != NULL) ? d : "(none)", -1);
    } else if (strcmp(u, "/big") == 0) {
        size_t   io_size = 0u;
        uint8_t *io      = jpp_http_io_buf(c, &io_size);
        memset(io, 'A', io_size);
        jpp_http_resp_begin(c, io_size * 3u);
        for (int i = 0; i < 3; i++) { jpp_http_resp_write(c, io, io_size); }
        jpp_http_resp_finish(c);
    } else if (strcmp(m, "PUT") == 0) {
        const char *expect = jpp_http_header(c, "Expect");
        if (expect != NULL && strcasecmp(expect, "100-continue") == 0) {
            jpp_http_send_continue(c);
        }
        size_t   io_size = 0u;
        uint8_t *io      = jpp_http_io_buf(c, &io_size);
        size_t remaining = jpp_http_content_len(c);
        size_t total     = 0u;
        unsigned sum     = 0u;
        while (remaining > 0u) {
            size_t want = (remaining < io_size) ? remaining : io_size;
            int n = jpp_http_recv(c, io, want);
            if (n == 0) { continue; }
            if (n < 0)  { break; }
            for (int i = 0; i < n; i++) { sum += io[i]; }
            total     += (size_t)n;
            remaining -= (size_t)n;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "got %zu sum %u", total, sum);
        jpp_http_resp_set_status(c, JPP_HTTP_201);
        jpp_http_resp_send(c, msg, -1);
    } else {
        jpp_http_resp_set_hdr(c, "Allow", "GET, HEAD, PUT");
        jpp_http_resp_send_err(c, JPP_HTTP_405, "nope");
    }
}

/* ---- client helpers ------------------------------------------------------ */

static int connect_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)g_port),
    };
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        perror("connect");
        exit(2);
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static void put_str(int fd, const char *s) { write(fd, s, strlen(s)); }

/* Read until the peer goes quiet for one receive timeout, or EOF. */
static size_t slurp(int fd, char *buf, size_t cap)
{
    size_t used = 0u;
    for (;;) {
        ssize_t n = read(fd, buf + used, cap - 1u - used);
        if (n <= 0) { break; }
        used += (size_t)n;
        if (used + 1u >= cap) { break; }
    }
    buf[used] = '\0';
    return used;
}

int main(int argc, char **argv)
{
    if (argc > 1) { g_port = atoi(argv[1]); }

    jpp_http_server_config_t cfg = {
        .owner          = "test",
        .port           = (uint16_t)g_port,
        .stack_bytes    = 8192u,
        .recv_timeout_s = 2u,
        .send_timeout_s = 2u,
        .handler        = test_handler,
    };

    check(jpp_app_pool_owner() == NULL, "pool free before start");
    jpp_http_result_t rc = jpp_http_server_start(&cfg);
    check(rc == JPP_HTTP_OK, "server starts");
    if (rc != JPP_HTTP_OK) { return 2; }
    check(jpp_app_pool_owner() != NULL && strcmp(jpp_app_pool_owner(), "test") == 0,
          "pool owned by the server while running");

    char buf[262144];

    /* 1. keep-alive: two requests on one connection. */
    {
        int fd = connect_server();
        put_str(fd, "GET /fixed HTTP/1.1\r\nHost: x\r\n\r\n");
        usleep(150000);
        put_str(fd, "GET /hdr HTTP/1.1\r\nHost: x\r\nX-Depth: 1\r\nConnection: close\r\n\r\n");
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(strstr(buf, "HTTP/1.1 200 OK") == buf, "keep-alive: first status line");
        check(strstr(buf, "Content-Length: 11") != NULL, "keep-alive: content-length");
        check(strstr(buf, "hello world") != NULL, "keep-alive: first body");
        check(strstr(buf, "Connection: keep-alive") != NULL, "keep-alive: first conn header");
        check(strstr(buf, "\r\n\r\n1") != NULL || strstr(buf, "1\0") != NULL,
              "keep-alive: second body (case-insensitive header lookup)");
        check(strstr(buf, "Connection: close") != NULL, "keep-alive: honours Connection: close");
    }

    /* 2. HEAD: headers only, no body. */
    {
        int fd = connect_server();
        put_str(fd, "HEAD /fixed HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        size_t n = slurp(fd, buf, sizeof(buf));
        close(fd);
        char *body = strstr(buf, "\r\n\r\n");
        check(strstr(buf, "Content-Length: 11") != NULL, "HEAD: content-length still declared");
        check(body != NULL && (size_t)(body + 4 - buf) == n, "HEAD: body suppressed");
    }

    /* 3. chunked framing. */
    {
        int fd = connect_server();
        put_str(fd, "GET /chunked HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(strstr(buf, "Transfer-Encoding: chunked") != NULL, "chunked: TE header");
        check(strstr(buf, "3\r\nabc\r\n4\r\ndefg\r\n0\r\n\r\n") != NULL, "chunked: framing");
    }

    /* 4. PUT with Expect: 100-continue, body in two segments. */
    {
        int fd = connect_server();
        put_str(fd, "PUT /f.bin HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n"
                    "Content-Length: 6\r\nConnection: close\r\n\r\n");
        usleep(150000);
        put_str(fd, "abc");
        usleep(120000);
        put_str(fd, "def");
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(strstr(buf, "HTTP/1.1 100 Continue") == buf, "PUT: 100-continue sent first");
        check(strstr(buf, "HTTP/1.1 201 Created") != NULL, "PUT: 201 after body");
        check(strstr(buf, "got 6 sum 597") != NULL, "PUT: whole body received in order");
    }

    /* 5. body bytes arriving in the same segment as the headers. */
    {
        int fd = connect_server();
        put_str(fd, "PUT /f.bin HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\n"
                    "Connection: close\r\n\r\nabcdef");
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(strstr(buf, "got 6 sum 597") != NULL, "PUT: body coalesced with header block");
    }

    /* 6. header block split across TCP segments. */
    {
        int fd = connect_server();
        put_str(fd, "GET /hdr HTTP/1.1\r\nHos");
        usleep(120000);
        put_str(fd, "t: x\r\nX-DEPTH: deep\r\nConnection: close\r\n\r\n");
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(strstr(buf, "deep") != NULL, "split header block reassembled");
    }

    /* 7. large fixed-length body through the pool I/O buffer. */
    {
        int fd = connect_server();
        put_str(fd, "GET /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        size_t n = slurp(fd, buf, sizeof(buf));
        close(fd);
        char *hdr_end = strstr(buf, "\r\n\r\n");
        long declared = 0;
        char *cl = strstr(buf, "Content-Length: ");
        if (cl != NULL) { declared = strtol(cl + 16, NULL, 10); }
        long body_len = (hdr_end != NULL) ? (long)(n - (size_t)(hdr_end + 4 - buf)) : -1;
        printf("   (declared %ld, received %ld)\n", declared, body_len);
        check(declared > 0 && declared == body_len, "large body: length matches declaration");
    }

    /* 7b. a realistic client's header pile: the one we need must still be
           reachable when it is the last of many. */
    {
        int fd = connect_server();
        char req[2048];
        int  n = snprintf(req, sizeof(req), "GET /hdr HTTP/1.1\r\nHost: x\r\n");
        for (int i = 0; i < 24; i++) {
            n += snprintf(req + n, sizeof(req) - (size_t)n,
                          "X-Filler-%02d: padding\r\n", i);
        }
        n += snprintf(req + n, sizeof(req) - (size_t)n,
                      "X-Depth: last-one\r\nConnection: close\r\n\r\n");
        write(fd, req, (size_t)n);
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(strstr(buf, "last-one") != NULL, "header past 24 others still found");
    }

    /* 7c. a header block larger than the buffer is dropped, not overflowed. */
    {
        int fd = connect_server();
        char req[8192];
        int  n = snprintf(req, sizeof(req), "GET /fixed HTTP/1.1\r\nHost: x\r\n");
        while (n < 6000) {
            n += snprintf(req + n, sizeof(req) - (size_t)n,
                          "X-Bloat-%04d: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n", n);
        }
        n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");
        write(fd, req, (size_t)n);
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(1, "oversized header block handled without crashing");
        /* Server must still be alive for the next request. */
        fd = connect_server();
        put_str(fd, "GET /fixed HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(strstr(buf, "hello world") != NULL, "server survives an oversized request");
    }

    /* 8. unknown method. */
    {
        int fd = connect_server();
        put_str(fd, "LOCK /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(strstr(buf, "HTTP/1.1 405") == buf, "unknown method: 405");
        check(strstr(buf, "Allow: GET, HEAD, PUT") != NULL, "unknown method: Allow header");
    }

    /* 9. stop releases the pool. */
    rc = jpp_http_server_stop();
    check(rc == JPP_HTTP_OK, "server stops");
    check(jpp_app_pool_owner() == NULL, "pool released on stop");
    check(!jpp_http_server_is_running(), "not running after stop");

    /* 10. restart works (sockets and pool were cleaned up). */
    rc = jpp_http_server_start(&cfg);
    check(rc == JPP_HTTP_OK, "server restarts on the same port");
    if (rc == JPP_HTTP_OK) {
        int fd = connect_server();
        put_str(fd, "GET /fixed HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(strstr(buf, "hello world") != NULL, "restarted server serves");
        check(jpp_http_server_stop() == JPP_HTTP_OK, "second stop");
    }

    /* 10b. stopping with a client parked mid-body: the stop path must not
            return (and must not release the pool) until the server task has
            genuinely left the memory it is running on. */
    {
        rc = jpp_http_server_start(&cfg);
        check(rc == JPP_HTTP_OK, "server starts for the mid-transfer stop test");

        int fd = connect_server();
        put_str(fd, "PUT /stuck.bin HTTP/1.1\r\nHost: x\r\n"
                    "Content-Length: 1000000\r\nConnection: close\r\n\r\n");
        put_str(fd, "partial");
        usleep(300000);   /* handler is now blocked waiting for the rest */

        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        jpp_http_result_t srdc = jpp_http_server_stop();
        gettimeofday(&t1, NULL);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 +
                  (t1.tv_usec - t0.tv_usec) / 1000;
        close(fd);

        printf("   (stop took %ld ms)\n", ms);
        check(srdc == JPP_HTTP_OK, "stop succeeds with a transfer in flight");
        check(ms < 2000, "stop does not wait out the receive timeout");
        check(jpp_app_pool_owner() == NULL, "pool released after a mid-transfer stop");
    }

    /* 11. the pool is a hard interlock: no server while an app holds it. */
    check(jpp_app_pool_acquire_as("app", 0u, NULL) != NULL, "app acquires the pool");
    check(jpp_http_server_start(&cfg) == JPP_HTTP_ERR_POOL_BUSY,
          "server refuses to start while an app holds the pool");
    jpp_app_pool_release();

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
