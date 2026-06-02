#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_FILESERVER_IP_MAX       16u   /* "255.255.255.255\0" */
#define JPP_FILESERVER_PASS_LEN     8u    /* random A-Za-z0-9 password length */
#define JPP_FILESERVER_PASS_MAX     24u   /* max password length (random or static) */
#define JPP_FILESERVER_DEFAULT_PORT 80u
#define JPP_FILESERVER_DEFAULT_ROOT "/sd"

typedef enum {
    JPP_FILESERVER_RESULT_OK = 0,
    JPP_FILESERVER_RESULT_INVALID_ARGUMENT,
    JPP_FILESERVER_RESULT_ALREADY_INITIALIZED,
    JPP_FILESERVER_RESULT_NOT_INITIALIZED,
    JPP_FILESERVER_RESULT_ALREADY_RUNNING,
    JPP_FILESERVER_RESULT_START_FAILED,
    JPP_FILESERVER_RESULT_STOP_FAILED,
} jpp_fileserver_result_t;

typedef enum {
    JPP_FILESERVER_STATE_STOPPED = 0,
    JPP_FILESERVER_STATE_RUNNING,
    JPP_FILESERVER_STATE_ERROR,
} jpp_fileserver_state_t;

typedef struct {
    uint16_t    port;
    const char *sd_root;  /* VFS path prefix served, default "/sd" */
} jpp_fileserver_config_t;

typedef struct {
    jpp_fileserver_state_t state;
    uint16_t               port;
    char                   ip[JPP_FILESERVER_IP_MAX];            /* dotted-decimal, "" if unknown */
    char                   password[JPP_FILESERVER_PASS_MAX + 1u]; /* "" when stopped */
} jpp_fileserver_status_t;

void                    jpp_fileserver_config_defaults(jpp_fileserver_config_t *config);
jpp_fileserver_result_t jpp_fileserver_init(const jpp_fileserver_config_t *config);
jpp_fileserver_result_t jpp_fileserver_start(void);
jpp_fileserver_result_t jpp_fileserver_start_with_password(const char *password);
jpp_fileserver_result_t jpp_fileserver_stop(void);
void                    jpp_fileserver_get_status(jpp_fileserver_status_t *status);
const char             *jpp_fileserver_state_name(jpp_fileserver_state_t state);
const char             *jpp_fileserver_result_name(jpp_fileserver_result_t result);

#ifdef __cplusplus
}
#endif
