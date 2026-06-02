#pragma once

/*
 * LRV interactive verification HTTP server (port 3000).
 *
 * Serves a verification page showing the device's LRV certificate and live
 * challenge-response signatures. Mutually exclusive with the WebDAV server.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpp_rtc_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_LRV_SERVER_PORT 3000u

typedef enum {
    JPP_LRV_SERVER_OK = 0,
    JPP_LRV_SERVER_ERR_NO_WIFI,
    JPP_LRV_SERVER_ERR_WEBDAV_RUNNING,
    JPP_LRV_SERVER_ERR_NOT_UNLOCKED,
    JPP_LRV_SERVER_ERR_INTERNAL,
} jpp_lrv_server_result_t;

/*
 * Start the LRV HTTP server on port 3000.
 * rtc may be NULL; if non-NULL it is used to generate timestamp challenges.
 * Returns ERR_WEBDAV_RUNNING if the WebDAV server is already active.
 * Returns ERR_NO_WIFI if the device has no IP address.
 * Returns ERR_NOT_UNLOCKED if LRV data has not been decrypted yet.
 */
jpp_lrv_server_result_t jpp_lrv_server_start(jpp_rtc_state_t *rtc);

/* Stop the LRV server if running. */
void jpp_lrv_server_stop(void);

/* Returns true while the server is accepting connections. */
bool jpp_lrv_server_is_running(void);

/* Write "x.x.x.x:3000" into out (NUL-terminated). */
void jpp_lrv_server_get_addr(char *out, size_t len);

#ifdef __cplusplus
}
#endif
