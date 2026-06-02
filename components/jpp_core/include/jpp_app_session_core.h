#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "jpp_sdk_bridge.h"
#include "jpp_ui_core.h"
#include "jpp_vm_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_APP_SESSION_APP_ID_MAX JPP_SDK_APP_ID_MAX
#define JPP_APP_SESSION_ERROR_MAX 32u

typedef enum {
    JPP_APP_SESSION_STATUS_OK = 0,
    JPP_APP_SESSION_STATUS_INVALID_ARGUMENT,
    JPP_APP_SESSION_STATUS_INVALID_STATE,
    JPP_APP_SESSION_STATUS_UI_ERROR,
    JPP_APP_SESSION_STATUS_VM_ERROR,
    JPP_APP_SESSION_STATUS_WATCHDOG_SUSPEND,
    JPP_APP_SESSION_STATUS_WATCHDOG_KILL,
} jpp_app_session_status_t;

typedef enum {
    JPP_APP_FAILURE_NONE = 0,
    JPP_APP_FAILURE_CREATE,
    JPP_APP_FAILURE_START,
    JPP_APP_FAILURE_FOREGROUND,
    JPP_APP_FAILURE_ACTION,
    JPP_APP_FAILURE_BACKGROUND,
    JPP_APP_FAILURE_SD,
} jpp_app_failure_phase_t;

typedef enum {
    JPP_APP_FAILURE_CODE_NONE = 0,
    JPP_APP_FAILURE_CODE_CREATE_EXCEPTION,
    JPP_APP_FAILURE_CODE_START_EXCEPTION,
    JPP_APP_FAILURE_CODE_FOREGROUND_EXCEPTION,
    JPP_APP_FAILURE_CODE_ACTION_EXCEPTION,
    JPP_APP_FAILURE_CODE_BACKGROUND_EXCEPTION,
    JPP_APP_FAILURE_CODE_VM_ERROR,
    JPP_APP_FAILURE_CODE_BACKGROUND_WATCHDOG,
    JPP_APP_FAILURE_CODE_SD_REMOVED,
} jpp_app_failure_code_t;

typedef struct {
    jpp_app_failure_phase_t phase;
    jpp_app_failure_code_t code;
    char exception_name[JPP_APP_SESSION_ERROR_MAX];
} jpp_app_failure_t;

typedef struct {
    char app_id[JPP_APP_SESSION_APP_ID_MAX];
    jpp_sdk_context_t *sdk;
    bool active;
    bool foreground;
    bool recovered_to_launcher;
    jpp_app_failure_t last_failure;
} jpp_app_session_t;

void jpp_app_session_init(jpp_app_session_t *session);
jpp_app_session_status_t jpp_app_session_start(
    jpp_app_session_t *session,
    jpp_ui_shell_t *shell,
    jpp_vm_context_t *vm_context,
    const char *task_name,
    const char *app_id,
    jpp_sdk_context_t *sdk
);
jpp_app_session_status_t jpp_app_session_recover_exception(
    jpp_app_session_t *session,
    jpp_ui_shell_t *shell,
    jpp_app_failure_phase_t phase,
    const char *exception_name
);
jpp_app_session_status_t jpp_app_session_recover_vm_status(
    jpp_app_session_t *session,
    jpp_ui_shell_t *shell,
    jpp_app_failure_phase_t phase,
    jpp_vm_status_t vm_status
);
jpp_app_session_status_t jpp_app_session_supervise_background(
    jpp_app_session_t *session,
    jpp_ui_shell_t *shell,
    jpp_vm_context_t *vm_context,
    const char *owner_task_name,
    size_t task_index,
    unsigned int elapsed_ms
);
jpp_app_session_status_t jpp_app_session_handle_sd_removed(
    jpp_app_session_t *session,
    jpp_ui_shell_t *shell
);

const char *jpp_app_session_status_name(jpp_app_session_status_t status);
const char *jpp_app_failure_phase_name(jpp_app_failure_phase_t phase);
const char *jpp_app_failure_code_name(jpp_app_failure_code_t code);

#ifdef __cplusplus
}
#endif
