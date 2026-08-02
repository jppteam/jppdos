/*
 * Host harness for the ported WebDAV server: runs the real
 * jpp_fileserver_core.c over a temp directory and drives it with real WebDAV
 * requests (Basic auth, OPTIONS, PROPFIND, GET, PUT, MOVE, MKCOL, DELETE).
 */
#include "jpp_fileserver_core.h"

#include <stdint.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int   g_fail = 0;
static int   g_port = 18081;
static char  g_root[256];
static char  g_auth[128];

static void check(int cond, const char *what)
{
    printf("%-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) { g_fail++; }
}

static int connect_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_port) };
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { perror("connect"); exit(2); }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 400000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

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

/* One request/response round trip on a fresh connection. */
static size_t request(const char *method, const char *uri, const char *extra_hdrs,
                      const void *body, size_t body_len, char *out, size_t cap)
{
    int  fd = connect_server();
    char head[512];
    int  n = snprintf(head, sizeof(head),
                      "%s %s HTTP/1.1\r\nHost: dev\r\n%s%s"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      method, uri, g_auth,
                      (extra_hdrs != NULL) ? extra_hdrs : "", body_len);
    write(fd, head, (size_t)n);
    if (body_len > 0u) { write(fd, body, body_len); }
    size_t got = slurp(fd, out, cap);
    close(fd);
    return got;
}

int main(int argc, char **argv)
{
    if (argc > 1) { g_port = atoi(argv[1]); }
    snprintf(g_root, sizeof(g_root), "%s",
             (argc > 2) ? argv[2] : "/tmp/jppd_webdav_test");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s/sub", g_root, g_root);
    if (system(cmd) != 0) { return 2; }
    snprintf(cmd, sizeof(cmd), "printf 'hello sd card' > %s/readme.txt", g_root);
    if (system(cmd) != 0) { return 2; }

    jpp_fileserver_config_t cfg = { .port = (uint16_t)g_port, .sd_root = g_root };
    check(jpp_fileserver_init(&cfg) == JPP_FILESERVER_RESULT_OK, "fileserver init");
    check(jpp_fileserver_start_with_password("secret") == JPP_FILESERVER_RESULT_OK,
          "fileserver starts");

    jpp_fileserver_status_t st;
    jpp_fileserver_get_status(&st);
    check(st.state == JPP_FILESERVER_STATE_RUNNING, "status reports running");
    check(strcmp(st.password, "secret") == 0, "status reports the static password");

    /* base64("jppd:secret") == "anBwZDpzZWNyZXQ=" */
    snprintf(g_auth, sizeof(g_auth), "Authorization: Basic anBwZDpzZWNyZXQ=\r\n");

    static char buf[262144];

    /* Unauthenticated request is challenged. */
    {
        int fd = connect_server();
        const char *r = "GET /readme.txt HTTP/1.1\r\nHost: d\r\nConnection: close\r\n\r\n";
        write(fd, r, strlen(r));
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(strstr(buf, "HTTP/1.1 401") == buf, "no credentials -> 401");
        check(strstr(buf, "WWW-Authenticate: Basic realm=\"WebDAV\"") != NULL,
              "401 carries the auth challenge");
    }

    /* Wrong password stays out. */
    {
        int fd = connect_server();
        const char *r = "GET /readme.txt HTTP/1.1\r\nHost: d\r\n"
                        "Authorization: Basic anBwZDp3cm9uZw==\r\nConnection: close\r\n\r\n";
        write(fd, r, strlen(r));
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(strstr(buf, "HTTP/1.1 401") == buf, "wrong password -> 401");
    }

    /* OPTIONS needs no credentials (clients probe before authenticating). */
    {
        int fd = connect_server();
        const char *r = "OPTIONS / HTTP/1.1\r\nHost: d\r\nConnection: close\r\n\r\n";
        write(fd, r, strlen(r));
        slurp(fd, buf, sizeof(buf));
        close(fd);
        check(strstr(buf, "HTTP/1.1 200") == buf, "OPTIONS unauthenticated -> 200");
        check(strstr(buf, "DAV: 1") != NULL, "OPTIONS advertises DAV: 1");
        check(strstr(buf, "MS-Author-Via: DAV") != NULL, "OPTIONS advertises MS-Author-Via");
    }

    /* GET a file. */
    request("GET", "/readme.txt", NULL, NULL, 0, buf, sizeof(buf));
    check(strstr(buf, "Content-Type: text/plain") != NULL, "GET file: mime type");
    check(strstr(buf, "Content-Length: 13") != NULL, "GET file: content length");
    check(strstr(buf, "hello sd card") != NULL, "GET file: body");

    /* HEAD the same file. */
    {
        size_t n = request("HEAD", "/readme.txt", NULL, NULL, 0, buf, sizeof(buf));
        char *body = strstr(buf, "\r\n\r\n");
        check(strstr(buf, "Content-Length: 13") != NULL, "HEAD file: length declared");
        check(body != NULL && (size_t)(body + 4 - buf) == n, "HEAD file: no body");
    }

    /* GET a directory renders the HTML index. */
    request("GET", "/", NULL, NULL, 0, buf, sizeof(buf));
    check(strstr(buf, "Transfer-Encoding: chunked") != NULL, "GET dir: chunked");
    check(strstr(buf, "readme.txt") != NULL, "GET dir: lists entries");
    check(strstr(buf, "0\r\n\r\n") != NULL, "GET dir: terminating chunk");

    /* PROPFIND Depth: 1 on the root. */
    request("PROPFIND", "/", "Depth: 1\r\n", NULL, 0, buf, sizeof(buf));
    check(strstr(buf, "HTTP/1.1 207 Multi-Status") == buf, "PROPFIND: 207");
    check(strstr(buf, "<D:multistatus xmlns:D=\"DAV:\">") != NULL, "PROPFIND: multistatus open");
    check(strstr(buf, "</D:multistatus>") != NULL, "PROPFIND: multistatus close");
    check(strstr(buf, "<D:href>/readme.txt</D:href>") != NULL, "PROPFIND: lists the file");
    check(strstr(buf, "<D:href>/sub/</D:href>") != NULL, "PROPFIND: lists the subdir");
    check(strstr(buf, "<D:getcontentlength>13</D:getcontentlength>") != NULL,
          "PROPFIND: reports the file size");

    /* PROPFIND Depth: 0 lists only the resource itself. */
    request("PROPFIND", "/", "Depth: 0\r\n", NULL, 0, buf, sizeof(buf));
    check(strstr(buf, "<D:href>/</D:href>") != NULL, "PROPFIND Depth 0: self");
    check(strstr(buf, "readme.txt") == NULL, "PROPFIND Depth 0: no children");

    /* Path traversal is refused. */
    request("GET", "/../etc/passwd", NULL, NULL, 0, buf, sizeof(buf));
    check(strstr(buf, "HTTP/1.1 400") == buf, "traversal (..) -> 400");

    request("GET", "/nope.txt", NULL, NULL, 0, buf, sizeof(buf));
    check(strstr(buf, "HTTP/1.1 404") == buf, "missing file -> 404");

    /* PUT a 100 KB file, then read it back and compare. */
    {
        size_t  len  = 100u * 1024u;
        char   *data = malloc(len);
        for (size_t i = 0u; i < len; i++) { data[i] = (char)('a' + (i % 26u)); }
        request("PUT", "/big.bin", NULL, data, len, buf, sizeof(buf));
        check(strstr(buf, "HTTP/1.1 201 Created") == buf, "PUT 100 KB: 201");

        char path[512];
        snprintf(path, sizeof(path), "%s/big.bin", g_root);
        struct stat sb;
        check(stat(path, &sb) == 0 && (size_t)sb.st_size == len,
              "PUT 100 KB: file written with the right size");

        static char got[262144];
        size_t n = request("GET", "/big.bin", NULL, NULL, 0, got, sizeof(got));
        char *body = strstr(got, "\r\n\r\n");
        size_t body_len = (body != NULL) ? n - (size_t)(body + 4 - got) : 0u;
        check(body_len == len, "GET 100 KB: full body returned");
        check(body != NULL && memcmp(body + 4, data, len) == 0,
              "GET 100 KB: bytes match what was PUT");
        free(data);
    }

    /* MKCOL / MOVE / DELETE. */
    request("MKCOL", "/newdir", NULL, NULL, 0, buf, sizeof(buf));
    check(strstr(buf, "HTTP/1.1 201") == buf, "MKCOL: 201");

    request("MOVE", "/readme.txt", "Destination: http://dev/newdir/moved.txt\r\n",
            NULL, 0, buf, sizeof(buf));
    check(strstr(buf, "HTTP/1.1 201") == buf, "MOVE: 201");
    {
        char path[512];
        struct stat sb;
        snprintf(path, sizeof(path), "%s/newdir/moved.txt", g_root);
        check(stat(path, &sb) == 0, "MOVE: file is at the destination");
    }

    request("DELETE", "/newdir", NULL, NULL, 0, buf, sizeof(buf));
    check(strstr(buf, "HTTP/1.1 204") == buf, "DELETE dir: 204");
    {
        char path[512];
        struct stat sb;
        snprintf(path, sizeof(path), "%s/newdir", g_root);
        check(stat(path, &sb) != 0, "DELETE dir: removed recursively");
    }

    check(jpp_fileserver_stop() == JPP_FILESERVER_RESULT_OK, "fileserver stops");
    jpp_fileserver_get_status(&st);
    check(st.state == JPP_FILESERVER_STATE_STOPPED, "status reports stopped");
    check(st.password[0] == '\0', "password cleared on stop");

    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_root);
    if (system(cmd) != 0) { /* best effort */ }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
