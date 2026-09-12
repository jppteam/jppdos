/*
 * Host harness for the FTP file server: runs the real jpp_fileserver_core.c
 * switched to FTP (so jpp_ftp_server_core.c underneath) over a temp directory
 * and drives it with real FTP commands over loopback — login, PASV/EPSV/PORT
 * data connections, LIST/NLST/MLSD, STOR/RETR with REST, and the file
 * management verbs — the way FileZilla, curl or Finder would.
 */
#include "jpp_fileserver_core.h"
#include "jpp_app_pool.h"

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
static int   g_port = 18021;
static char  g_root[256];
static int   g_ctrl = -1;

static void check(int cond, const char *what)
{
    printf("%-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) { g_fail++; }
}

static int connect_port(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { perror("connect"); exit(2); }
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/* Read one CRLF-terminated reply line (multi-line replies: keep reading until
   the "NNN " terminator line). */
static char g_reply[4096];

static int read_line(int fd, char *out, size_t cap)
{
    size_t used = 0u;
    while (used + 1u < cap) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n <= 0) { out[used] = '\0'; return -1; }
        if (ch == '\n') { break; }
        if (ch != '\r') { out[used++] = ch; }
    }
    out[used] = '\0';
    return (int)used;
}

static const char *reply(void)
{
    char line[512];
    g_reply[0] = '\0';
    if (read_line(g_ctrl, line, sizeof(line)) < 0) { return g_reply; }
    strncat(g_reply, line, sizeof(g_reply) - strlen(g_reply) - 1u);
    if (strlen(line) >= 4u && line[3] == '-') {
        char code[4] = { line[0], line[1], line[2], '\0' };
        for (;;) {
            if (read_line(g_ctrl, line, sizeof(line)) < 0) { break; }
            strncat(g_reply, "\n", sizeof(g_reply) - strlen(g_reply) - 1u);
            strncat(g_reply, line, sizeof(g_reply) - strlen(g_reply) - 1u);
            if (strncmp(line, code, 3) == 0 && line[3] == ' ') { break; }
        }
    }
    return g_reply;
}

static const char *cmd(const char *text)
{
    char buf[600];
    int  n = snprintf(buf, sizeof(buf), "%s\r\n", text);
    write(g_ctrl, buf, (size_t)n);
    return reply();
}

static int starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Parse "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)" and connect. */
static int pasv_connect(void)
{
    const char *r = cmd("PASV");
    unsigned h[4], p[2];
    const char *paren = strchr(r, '(');
    if (!starts(r, "227") || paren == NULL ||
        sscanf(paren + 1, "%u,%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3], &p[0], &p[1]) != 6) {
        printf("bad PASV reply: %s\n", r);
        return -1;
    }
    return connect_port((int)((p[0] << 8) | p[1]));
}

/* Parse "229 Entering Extended Passive Mode (|||port|)" and connect. */
static int epsv_connect(void)
{
    const char *r = cmd("EPSV");
    unsigned port = 0u;
    const char *bar = strstr(r, "(|||");
    if (!starts(r, "229") || bar == NULL || sscanf(bar + 4, "%u", &port) != 1) {
        printf("bad EPSV reply: %s\n", r);
        return -1;
    }
    return connect_port((int)port);
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

/* Run a listing-style command over a fresh PASV data connection. */
static size_t list_via_pasv(const char *command, char *out, size_t cap)
{
    int data = pasv_connect();
    if (data < 0) { out[0] = '\0'; return 0u; }
    const char *r = cmd(command);
    if (!starts(r, "150")) {
        printf("no 150 for %s: %s\n", command, r);
        close(data);
        out[0] = '\0';
        return 0u;
    }
    size_t n = slurp(data, out, cap);
    close(data);
    reply();  /* 226 */
    return n;
}

int main(int argc, char **argv)
{
    if (argc > 1) { g_port = atoi(argv[1]); }
    snprintf(g_root, sizeof(g_root), "%s",
             (argc > 2) ? argv[2] : "/tmp/jppd_ftp_test");
    char sh[512];
    snprintf(sh, sizeof(sh), "rm -rf %s && mkdir -p %s/sub", g_root, g_root);
    if (system(sh) != 0) { return 2; }
    snprintf(sh, sizeof(sh), "printf 'hello sd card' > %s/readme.txt", g_root);
    if (system(sh) != 0) { return 2; }

    jpp_fileserver_config_t cfg = { .port = 0u, .ftp_port = (uint16_t)g_port, .sd_root = g_root };
    check(jpp_fileserver_init(&cfg) == JPP_FILESERVER_RESULT_OK, "fileserver init");
    check(jpp_fileserver_get_protocol() == JPP_FILESERVER_PROTO_WEBDAV,
          "WebDAV is the default protocol");
    check(jpp_fileserver_set_protocol(JPP_FILESERVER_PROTO_FTP) == JPP_FILESERVER_RESULT_OK,
          "protocol switches to FTP while stopped");
    check(jpp_fileserver_start_with_password("secret") == JPP_FILESERVER_RESULT_OK,
          "FTP server starts");
    check(jpp_fileserver_set_protocol(JPP_FILESERVER_PROTO_WEBDAV) == JPP_FILESERVER_RESULT_RUNNING,
          "protocol cannot change while running");
    check(jpp_app_pool_in_use() && strcmp(jpp_app_pool_owner(), "ftp") == 0,
          "FTP server holds the app pool as \"ftp\"");

    jpp_fileserver_status_t st;
    jpp_fileserver_get_status(&st);
    check(st.state == JPP_FILESERVER_STATE_RUNNING, "status reports running");
    check(st.protocol == JPP_FILESERVER_PROTO_FTP, "status reports FTP");
    check(st.port == (uint16_t)g_port, "status reports the FTP port");
    check(strcmp(st.password, "secret") == 0, "status reports the static password");

    static char buf[262144];

    /* ---- Login ---- */
    g_ctrl = connect_port(g_port);
    check(starts(reply(), "220"), "greeting is 220");
    check(starts(cmd("PWD"), "530"), "commands before login -> 530");
    check(starts(cmd("USER jppd"), "331"), "USER -> 331");
    check(starts(cmd("PASS wrong"), "530"), "wrong password -> 530");
    check(starts(cmd("USER nobody"), "331"), "unknown USER still -> 331 (no enumeration)");
    check(starts(cmd("PASS secret"), "530"), "right password, wrong user -> 530");
    check(starts(cmd("USER jppd"), "331"), "USER jppd again -> 331");
    check(starts(cmd("PASS secret"), "230"), "correct login -> 230");

    /* ---- Session basics ---- */
    check(starts(cmd("SYST"), "215 UNIX"), "SYST -> 215 UNIX");
    {
        const char *r = cmd("FEAT");
        check(starts(r, "211-") && strstr(r, "\n MLSD") != NULL && strstr(r, "\n211 End") != NULL,
              "FEAT is multi-line and advertises MLSD");
        check(strstr(r, " REST STREAM") != NULL && strstr(r, " UTF8") != NULL,
              "FEAT advertises REST STREAM and UTF8");
    }
    check(starts(cmd("OPTS UTF8 ON"), "200"), "OPTS UTF8 ON -> 200");
    check(starts(cmd("TYPE I"), "200"), "TYPE I -> 200");
    check(starts(cmd("TYPE A"), "200"), "TYPE A -> 200");
    check(starts(cmd("TYPE X"), "504"), "TYPE X -> 504");
    check(starts(cmd("AUTH TLS"), "502"), "AUTH TLS -> 502 (no FTPS, client falls back)");
    check(starts(cmd("NOOP"), "200"), "NOOP -> 200");
    check(starts(cmd("BOGUS"), "500"), "unknown command -> 500");
    check(strcmp(cmd("PWD"), "257 \"/\" is the current directory") == 0, "PWD at root");

    /* ---- Navigation ---- */
    check(starts(cmd("CWD sub"), "250"), "CWD sub -> 250");
    check(strcmp(cmd("PWD"), "257 \"/sub\" is the current directory") == 0, "PWD in /sub");
    check(starts(cmd("CDUP"), "250"), "CDUP -> 250");
    check(strcmp(cmd("PWD"), "257 \"/\" is the current directory") == 0, "PWD back at root");
    check(starts(cmd("CWD ../../.."), "250"), "CWD ../../.. clamps rather than errors");
    check(strcmp(cmd("PWD"), "257 \"/\" is the current directory") == 0,
          "traversal clamps at the served root");
    check(starts(cmd("CWD nope"), "550"), "CWD to a missing dir -> 550");
    check(starts(cmd("CWD readme.txt"), "550"), "CWD to a file -> 550");
    check(starts(cmd("CWD /sub/"), "250"), "CWD with trailing slash");
    check(strcmp(cmd("PWD"), "257 \"/sub\" is the current directory") == 0,
          "trailing slash normalised away");
    check(starts(cmd("CWD /"), "250"), "CWD / -> 250");

    /* ---- Listings ---- */
    list_via_pasv("LIST", buf, sizeof(buf));
    check(strstr(buf, "readme.txt\r\n") != NULL, "LIST names the file");
    check(strstr(buf, "-rw-rw-r-- 1 jppd jppd") != NULL, "LIST: file line is ls -l shaped");
    check(strstr(buf, "drwxrwxr-x 1 jppd jppd") != NULL && strstr(buf, " sub\r\n") != NULL,
          "LIST: directory line is ls -l shaped");
    check(strstr(buf, "         13 ") != NULL, "LIST: size column");

    list_via_pasv("NLST", buf, sizeof(buf));
    check(strcmp(buf, "readme.txt\r\nsub\r\n") == 0 || strcmp(buf, "sub\r\nreadme.txt\r\n") == 0,
          "NLST: bare names only");

    list_via_pasv("MLSD", buf, sizeof(buf));
    check(strstr(buf, "type=file;size=13;modify=") != NULL && strstr(buf, "; readme.txt\r\n") != NULL,
          "MLSD: file facts");
    check(strstr(buf, "type=dir;modify=") != NULL && strstr(buf, "; sub\r\n") != NULL,
          "MLSD: directory facts");

    list_via_pasv("LIST -la", buf, sizeof(buf));
    check(strstr(buf, "readme.txt") != NULL, "LIST -la: ls options are ignored");

    list_via_pasv("LIST readme.txt", buf, sizeof(buf));
    check(strstr(buf, "readme.txt") != NULL && strstr(buf, "sub") == NULL,
          "LIST <file>: single entry");

    check(starts(cmd("LIST"), "425"), "LIST without PASV/PORT -> 425");

    {
        const char *r = cmd("MLST readme.txt");
        check(starts(r, "250-") && strstr(r, "\n type=file;size=13;modify=") != NULL &&
              strstr(r, "; /readme.txt\n250 End") != NULL,
              "MLST: facts on the control connection with the full path");
    }
    check(strcmp(cmd("SIZE readme.txt"), "213 13") == 0, "SIZE -> 213 13");
    check(starts(cmd("SIZE sub"), "550"), "SIZE on a dir -> 550");
    {
        const char *r = cmd("MDTM readme.txt");
        check(starts(r, "213 ") && strlen(r) == 4u + 14u, "MDTM -> 213 YYYYMMDDHHMMSS");
    }

    /* ---- RETR via EPSV ---- */
    {
        int data = epsv_connect();
        check(data >= 0, "EPSV -> 229 with a port");
        check(starts(cmd("RETR readme.txt"), "150"), "RETR -> 150");
        size_t n = slurp(data, buf, sizeof(buf));
        close(data);
        check(n == 13u && memcmp(buf, "hello sd card", 13u) == 0, "RETR: body matches the file");
        check(starts(reply(), "226"), "RETR -> 226 after the data connection closes");
    }
    check(starts(cmd("RETR nope.txt"), "550"), "RETR missing file -> 550");
    check(starts(cmd("RETR sub"), "550"), "RETR a directory -> 550");

    /* ---- STOR 100 KB via PASV, then RETR and compare ---- */
    size_t  len  = 100u * 1024u;
    char   *data = malloc(len);
    for (size_t i = 0u; i < len; i++) { data[i] = (char)('a' + (i % 26u)); }
    {
        int dfd = pasv_connect();
        check(starts(cmd("STOR big.bin"), "150"), "STOR -> 150");
        size_t sent = 0u;
        while (sent < len) {
            ssize_t w = write(dfd, data + sent, len - sent);
            if (w <= 0) { break; }
            sent += (size_t)w;
        }
        close(dfd);
        check(starts(reply(), "226"), "STOR -> 226");
        char path[512];
        snprintf(path, sizeof(path), "%s/big.bin", g_root);
        struct stat sb;
        check(stat(path, &sb) == 0 && (size_t)sb.st_size == len,
              "STOR 100 KB: file written with the right size");
    }
    {
        int dfd = pasv_connect();
        check(starts(cmd("RETR big.bin"), "150"), "RETR 100 KB -> 150");
        static char got[262144];
        size_t n = slurp(dfd, got, sizeof(got));
        close(dfd);
        reply();
        check(n == len, "RETR 100 KB: full body returned");
        check(memcmp(got, data, len) == 0, "RETR 100 KB: bytes match what was STORed");
    }

    /* ---- REST resume on RETR ---- */
    {
        check(strcmp(cmd("REST 102300"), "350 Restart position accepted (102300).") == 0,
              "REST -> 350");
        int dfd = pasv_connect();
        check(starts(cmd("RETR big.bin"), "150"), "RETR after REST -> 150");
        size_t n = slurp(dfd, buf, sizeof(buf));
        close(dfd);
        reply();
        check(n == len - 102300u && memcmp(buf, data + 102300u, n) == 0,
              "REST + RETR: only the tail is sent");
    }

    /* ---- APPE ---- */
    {
        int dfd = pasv_connect();
        check(starts(cmd("APPE readme.txt"), "150"), "APPE -> 150");
        write(dfd, "!!", 2);
        close(dfd);
        check(starts(reply(), "226"), "APPE -> 226");
        check(strcmp(cmd("SIZE readme.txt"), "213 15") == 0, "APPE grew the file by 2 bytes");
    }

    /* ---- Active mode (PORT): the harness listens, the server connects ---- */
    {
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in la = { .sin_family = AF_INET, .sin_port = 0 };
        la.sin_addr.s_addr = inet_addr("127.0.0.1");
        socklen_t llen = sizeof(la);
        check(bind(ls, (struct sockaddr *)&la, sizeof(la)) == 0 && listen(ls, 1) == 0 &&
              getsockname(ls, (struct sockaddr *)&la, &llen) == 0, "harness opens an active-mode listener");
        unsigned lport = ntohs(la.sin_port);
        char portcmd[64];
        snprintf(portcmd, sizeof(portcmd), "PORT 127,0,0,1,%u,%u", lport >> 8, lport & 255u);
        check(starts(cmd(portcmd), "200"), "PORT -> 200");
        check(starts(cmd("NLST"), "150"), "NLST in active mode -> 150");
        int dfd = accept(ls, NULL, NULL);
        check(dfd >= 0, "server connected back to the PORT address");
        size_t n = slurp(dfd, buf, sizeof(buf));
        close(dfd);
        close(ls);
        check(n > 0u && strstr(buf, "big.bin") != NULL, "active-mode listing arrived");
        check(starts(reply(), "226"), "active-mode NLST -> 226");

        /* FTP bounce: a PORT pointing at another host is refused. */
        check(starts(cmd("PORT 10,0,0,1,4,1"), "500"), "PORT to a foreign address -> 500");
        check(starts(cmd("PORT 127,0,0,1,0,80"), "500"), "PORT to a privileged port -> 500");
        snprintf(portcmd, sizeof(portcmd), "EPRT |1|127.0.0.1|%u|", lport);
        check(starts(cmd(portcmd), "200"), "EPRT -> 200");
        check(starts(cmd("EPRT |2|::1|5000|"), "522"), "EPRT IPv6 -> 522");
    }

    /* ---- File management ---- */
    check(strcmp(cmd("MKD newdir"), "257 \"/newdir\" created") == 0, "MKD -> 257");
    check(starts(cmd("MKD newdir"), "550"), "MKD existing -> 550");
    check(starts(cmd("RNFR readme.txt"), "350"), "RNFR -> 350");
    check(starts(cmd("RNTO newdir/moved.txt"), "250"), "RNTO -> 250");
    {
        char path[512];
        struct stat sb;
        snprintf(path, sizeof(path), "%s/newdir/moved.txt", g_root);
        check(stat(path, &sb) == 0, "RNFR/RNTO: file is at the destination");
    }
    check(starts(cmd("RNTO x"), "503"), "RNTO without RNFR -> 503");
    check(starts(cmd("RNFR nope"), "550"), "RNFR missing -> 550");
    check(starts(cmd("RMD newdir"), "550"), "RMD non-empty dir -> 550");
    check(starts(cmd("DELE newdir/moved.txt"), "250"), "DELE -> 250");
    check(starts(cmd("DELE newdir"), "550"), "DELE on a dir -> 550");
    check(starts(cmd("RMD newdir"), "250"), "RMD empty dir -> 250");
    {
        char path[512];
        struct stat sb;
        snprintf(path, sizeof(path), "%s/newdir", g_root);
        check(stat(path, &sb) != 0, "RMD: directory removed");
    }
    check(starts(cmd("DELE /../../etc/passwd"), "550"),
          "DELE with traversal resolves inside the root (-> 550, not found)");
    {
        int dfd = pasv_connect();
        check(starts(cmd("STOR /../../escape.txt"), "150"), "STOR with traversal is accepted...");
        write(dfd, "x", 1);
        close(dfd);
        reply();
        char inside[512], outside[512];
        struct stat sb;
        snprintf(inside,  sizeof(inside),  "%s/escape.txt",    g_root);
        snprintf(outside, sizeof(outside), "%s/../escape.txt", g_root);
        check(stat(inside, &sb) == 0 && stat(outside, &sb) != 0,
              "...but lands inside the root, never outside it");
    }

    /* ---- A second client is turned away, not left hanging ---- */
    {
        int second = connect_port(g_port);
        /* Give the server's command wait a chance to notice the listener. */
        (void)cmd("NOOP");
        char line[256];
        int n = read_line(second, line, sizeof(line));
        close(second);
        check(n > 0 && starts(line, "421"), "second client gets 421 immediately");
        check(starts(cmd("NOOP"), "200"), "first client is unaffected");
    }

    /* ---- Pipelined commands survive the line buffer ---- */
    {
        write(g_ctrl, "NOOP\r\nSYST\r\n", 12);
        check(starts(reply(), "200"), "pipelined: first reply");
        check(starts(reply(), "215"), "pipelined: second reply");
    }

    check(starts(cmd("QUIT"), "221"), "QUIT -> 221");
    close(g_ctrl);

    /* ---- Reconnect: session state was reset ---- */
    g_ctrl = connect_port(g_port);
    reply();
    check(starts(cmd("PWD"), "530"), "new session starts logged out");
    cmd("USER jppd");
    check(starts(cmd("PASS secret"), "230"), "new session can log in");
    check(strcmp(cmd("PWD"), "257 \"/\" is the current directory") == 0,
          "new session starts at the root");

    /* ---- Stop while a client is connected: the pool must come back ---- */
    check(jpp_fileserver_stop() == JPP_FILESERVER_RESULT_OK, "fileserver stops with a client attached");
    check(!jpp_app_pool_in_use(), "pool released on stop");
    close(g_ctrl);
    jpp_fileserver_get_status(&st);
    check(st.state == JPP_FILESERVER_STATE_STOPPED, "status reports stopped");
    check(st.password[0] == '\0', "password cleared on stop");
    check(st.protocol == JPP_FILESERVER_PROTO_FTP, "protocol selection survives a stop");

    /* ---- Switch back to WebDAV: the other backend starts on the other port ---- */
    check(jpp_fileserver_set_protocol(JPP_FILESERVER_PROTO_WEBDAV) == JPP_FILESERVER_RESULT_OK,
          "protocol switches back to WebDAV");
    jpp_fileserver_get_status(&st);
    check(st.port == JPP_FILESERVER_DEFAULT_PORT, "status reports the WebDAV port once selected");

    free(data);
    snprintf(sh, sizeof(sh), "rm -rf %s", g_root);
    if (system(sh) != 0) { /* best effort */ }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
