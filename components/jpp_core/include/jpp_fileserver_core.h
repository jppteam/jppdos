#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * jpp_fileserver_core — the File Server built-in app's backend.  One switch
 * (`jpp_fileserver_set_protocol`) picks which wire protocol the next start
 * speaks: WebDAV (the handlers in this file, on jpp_http_server_core) or FTP
 * (jpp_ftp_server_core).  Both serve the same root with the same one user,
 * `JPP_FILESERVER_USER` + the random-or-static password, and both run out of
 * the shared app pool — so whichever is chosen, the server is a foreground
 * activity that holds the pool while it is up.
 */

#define JPP_FILESERVER_IP_MAX       16u   /* "255.255.255.255\0" */
#define JPP_FILESERVER_PASS_LEN     8u    /* random A-Za-z0-9 password length */
#define JPP_FILESERVER_PASS_MAX     24u   /* max password length (random or static) */
#define JPP_FILESERVER_DEFAULT_PORT     80u   /* WebDAV (HTTP) */
#define JPP_FILESERVER_DEFAULT_FTP_PORT 21u
#define JPP_FILESERVER_DEFAULT_ROOT "/sd"
#define JPP_FILESERVER_USER         "jppd"  /* the one account, both protocols */

typedef enum {
    JPP_FILESERVER_RESULT_OK = 0,
    JPP_FILESERVER_RESULT_ALREADY_INITIALIZED,
    JPP_FILESERVER_RESULT_NOT_INITIALIZED,
    JPP_FILESERVER_RESULT_START_FAILED,
    JPP_FILESERVER_RESULT_STOP_FAILED,
    JPP_FILESERVER_RESULT_INVALID_ARGUMENT,
    JPP_FILESERVER_RESULT_RUNNING,        /* refused because a server is up */
} jpp_fileserver_result_t;

typedef enum {
    JPP_FILESERVER_STATE_STOPPED = 0,
    JPP_FILESERVER_STATE_RUNNING,
    JPP_FILESERVER_STATE_ERROR,
} jpp_fileserver_state_t;

/* Wire protocol.  Numeric values are persisted (NVS `jpp_webdav`/`protocol`),
   so they are append-only. */
typedef enum {
    JPP_FILESERVER_PROTO_WEBDAV = 0,
    JPP_FILESERVER_PROTO_FTP    = 1,
    JPP_FILESERVER_PROTO_COUNT,
} jpp_fileserver_protocol_t;

typedef struct {
    uint16_t    port;      /* WebDAV (HTTP) port, default 80 */
    uint16_t    ftp_port;  /* FTP control port, default 21   */
    const char *sd_root;   /* VFS path prefix served, default "/sd" */
} jpp_fileserver_config_t;

typedef struct {
    jpp_fileserver_state_t    state;
    jpp_fileserver_protocol_t protocol;   /* of the running server, else the selected one */
    uint16_t                  port;       /* of that protocol */
    char                      ip[JPP_FILESERVER_IP_MAX];            /* dotted-decimal, "" if unknown */
    char                      password[JPP_FILESERVER_PASS_MAX + 1u]; /* "" when stopped */
} jpp_fileserver_status_t;

void                    jpp_fileserver_config_defaults(jpp_fileserver_config_t *config);
jpp_fileserver_result_t jpp_fileserver_init(const jpp_fileserver_config_t *config);
/* Select the protocol the *next* start uses.  Refused (RESULT_RUNNING) while
   a server is up — stop it first. */
jpp_fileserver_result_t   jpp_fileserver_set_protocol(jpp_fileserver_protocol_t protocol);
jpp_fileserver_protocol_t jpp_fileserver_get_protocol(void);
/* "WebDAV" / "FTP" — the label the UI shows. */
const char               *jpp_fileserver_protocol_name(jpp_fileserver_protocol_t protocol);
jpp_fileserver_result_t jpp_fileserver_start(void);
jpp_fileserver_result_t jpp_fileserver_start_with_password(const char *password);
jpp_fileserver_result_t jpp_fileserver_stop(void);
void                    jpp_fileserver_get_status(jpp_fileserver_status_t *status);
const char             *jpp_fileserver_state_name(jpp_fileserver_state_t state);
const char             *jpp_fileserver_result_name(jpp_fileserver_result_t result);

#ifdef __cplusplus
}
#endif
