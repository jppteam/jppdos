#include "../include/jpp_app_session_core.h"
#include "../include/jpp_string_util.h"

#include <stdio.h>
#include <string.h>

static const char *JPP_APP_SESSION_SCREEN = "runtime_app";

static jpp_app_failure_code_t jpp_app_code_for_phase(jpp_app_failure_phase_t phase)
{
    switch (phase) {
    case JPP_APP_FAILURE_CREATE:
        return JPP_APP_FAILURE_CODE_CREATE_EXCEPTION;
    case JPP_APP_FAILURE_START:
        return JPP_APP_FAILURE_CODE_START_EXCEPTION;
    case JPP_APP_FAILURE_FOREGROUND:
        return JPP_APP_FAILURE_CODE_FOREGROUND_EXCEPTION;
    case JPP_APP_FAILURE_ACTION:
        return JPP_APP_FAILURE_CODE_ACTION_EXCEPTION;
    case JPP_APP_FAILURE_BACKGROUND:
        return JPP_APP_FAILURE_CODE_BACKGROUND_EXCEPTION;
    case JPP_APP_FAILURE_SD:
        return JPP_APP_FAILURE_CODE_SD_REMOVED;
    case JPP_APP_FAILURE_NONE:
        return JPP_APP_FAILURE_CODE_NONE;
    }
    return JPP_APP_FAILURE_CODE_VM_ERROR;
}

static jpp_app_session_status_t jpp_app_session_recover(
    jpp_app_session_t *session,
    jpp_ui_shell_t *shell,
    jpp_app_failure_phase_t phase,
    jpp_app_failure_code_t code,
    const char *exception_name
)
{
    const char *error_name;
    jpp_ui_status_t ui_status;

    if (session == NULL || shell == NULL || code == JPP_APP_FAILURE_CODE_NONE) {
        return JPP_APP_SESSION_STATUS_INVALID_ARGUMENT;
    }
    session->active = false;
    session->foreground = false;
    if (session->sdk != NULL) {
        session->sdk->close_requested = true;
    }
    session->last_failure.phase = phase;
    session->last_failure.code = code;
    jpp_str_copy(
        session->last_failure.exception_name,
        sizeof(session->last_failure.exception_name),
        jpp_str_nonempty(exception_name) ? exception_name : jpp_app_failure_code_name(code)
    );
    error_name = jpp_app_failure_code_name(code);
    ui_status = jpp_ui_shell_record_crash(shell, session->app_id, error_name);
    if (ui_status != JPP_UI_STATUS_OK) {
        return JPP_APP_SESSION_STATUS_UI_ERROR;
    }
    (void)snprintf(
        shell->crash_log,
        sizeof(shell->crash_log),
        "app_id=%s\nphase=%s\ncode=%s\nerror=%s\nlog=/data/ui_crash.log\n",
        session->app_id,
        jpp_app_failure_phase_name(phase),
        error_name,
        session->last_failure.exception_name
    );
    session->recovered_to_launcher = true;
    return JPP_APP_SESSION_STATUS_OK;
}

void jpp_app_session_init(jpp_app_session_t *session)
{
    if (session == NULL) {
        return;
    }
    memset(session, 0, sizeof(*session));
}

jpp_app_session_status_t jpp_app_session_start(
    jpp_app_session_t *session,
    jpp_ui_shell_t *shell,
    jpp_vm_context_t *vm_context,
    const char *task_name,
    const char *app_id,
    jpp_sdk_context_t *sdk
)
{
    jpp_vm_request_t request;
    jpp_vm_status_t vm_status;
    jpp_ui_status_t ui_status;

    if (session == NULL || shell == NULL || vm_context == NULL || !jpp_str_nonempty(task_name) ||
        !jpp_str_nonempty(app_id) || sdk == NULL) {
        return JPP_APP_SESSION_STATUS_INVALID_ARGUMENT;
    }
    jpp_app_session_init(session);
    jpp_str_copy(session->app_id, sizeof(session->app_id), app_id);
    session->sdk = sdk;
    memset(&request, 0, sizeof(request));
    request.kind = JPP_VM_REQUEST_FOREGROUND;
    jpp_str_copy(request.app_id, sizeof(request.app_id), app_id);
    vm_status = jpp_vm_schedule_request(vm_context, task_name, &request);
    if (vm_status != JPP_VM_STATUS_OK) {
        return jpp_app_session_recover_vm_status(session, shell, JPP_APP_FAILURE_FOREGROUND, vm_status);
    }
    ui_status = jpp_ui_stack_push(&shell->stack, JPP_APP_SESSION_SCREEN);
    if (ui_status != JPP_UI_STATUS_OK) {
        return JPP_APP_SESSION_STATUS_UI_ERROR;
    }
    session->active = true;
    session->foreground = true;
    return JPP_APP_SESSION_STATUS_OK;
}

jpp_app_session_status_t jpp_app_session_recover_exception(
    jpp_app_session_t *session,
    jpp_ui_shell_t *shell,
    jpp_app_failure_phase_t phase,
    const char *exception_name
)
{
    if (!jpp_str_nonempty(exception_name)) {
        return JPP_APP_SESSION_STATUS_INVALID_ARGUMENT;
    }
    return jpp_app_session_recover(session, shell, phase, jpp_app_code_for_phase(phase), exception_name);
}

jpp_app_session_status_t jpp_app_session_recover_vm_status(
    jpp_app_session_t *session,
    jpp_ui_shell_t *shell,
    jpp_app_failure_phase_t phase,
    jpp_vm_status_t vm_status
)
{
    if (vm_status == JPP_VM_STATUS_OK) {
        return JPP_APP_SESSION_STATUS_OK;
    }
    return jpp_app_session_recover(session, shell, phase, JPP_APP_FAILURE_CODE_VM_ERROR, jpp_vm_status_name(vm_status));
}

jpp_app_session_status_t jpp_app_session_supervise_background(
    jpp_app_session_t *session,
    jpp_ui_shell_t *shell,
    jpp_vm_context_t *vm_context,
    const char *owner_task_name,
    size_t task_index,
    unsigned int elapsed_ms
)
{
    jpp_sdk_background_task_t *task;
    jpp_vm_request_t request;
    jpp_vm_status_t vm_status;

    if (session == NULL || session->sdk == NULL || shell == NULL || vm_context == NULL ||
        !jpp_str_nonempty(owner_task_name)) {
        return JPP_APP_SESSION_STATUS_INVALID_ARGUMENT;
    }
    if (!session->active || task_index >= session->sdk->background_task_count) {
        return JPP_APP_SESSION_STATUS_INVALID_STATE;
    }
    task = &session->sdk->background_tasks[task_index];
    vm_status = jpp_vm_owner_dequeue_request(vm_context, owner_task_name, &request);
    if (vm_status != JPP_VM_STATUS_OK) {
        return jpp_app_session_recover_vm_status(session, shell, JPP_APP_FAILURE_BACKGROUND, vm_status);
    }
    if (request.kind != JPP_VM_REQUEST_BACKGROUND || strcmp(request.app_id, session->app_id) != 0 ||
        strcmp(request.module_name, task->name) != 0) {
        return jpp_app_session_recover(
            session,
            shell,
            JPP_APP_FAILURE_BACKGROUND,
            JPP_APP_FAILURE_CODE_VM_ERROR,
            "BACKGROUND_REQUEST_MISMATCH"
        );
    }
    if (elapsed_ms <= JPP_RESOURCE_BACKGROUND_CALLBACK_QUOTA_MS) {
        task->strikes = 0u;
        return JPP_APP_SESSION_STATUS_OK;
    }
    task->strikes += 1u;
    if (task->strikes < JPP_RESOURCE_BACKGROUND_WATCHDOG_STRIKES) {
        return JPP_APP_SESSION_STATUS_WATCHDOG_SUSPEND;
    }
    return jpp_app_session_recover(
        session,
        shell,
        JPP_APP_FAILURE_BACKGROUND,
        JPP_APP_FAILURE_CODE_BACKGROUND_WATCHDOG,
        "APP_BG_KILL"
    ) == JPP_APP_SESSION_STATUS_OK ? JPP_APP_SESSION_STATUS_WATCHDOG_KILL : JPP_APP_SESSION_STATUS_UI_ERROR;
}

jpp_app_session_status_t jpp_app_session_handle_sd_removed(
    jpp_app_session_t *session,
    jpp_ui_shell_t *shell
)
{
    if (session == NULL || shell == NULL) {
        return JPP_APP_SESSION_STATUS_INVALID_ARGUMENT;
    }
    if (!session->active) {
        return JPP_APP_SESSION_STATUS_OK;
    }
    return jpp_app_session_recover(
        session,
        shell,
        JPP_APP_FAILURE_SD,
        JPP_APP_FAILURE_CODE_SD_REMOVED,
        "SD_REMOVED"
    );
}

const char *jpp_app_session_status_name(jpp_app_session_status_t status)
{
    switch (status) {
    case JPP_APP_SESSION_STATUS_OK:
        return "OK";
    case JPP_APP_SESSION_STATUS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case JPP_APP_SESSION_STATUS_INVALID_STATE:
        return "INVALID_STATE";
    case JPP_APP_SESSION_STATUS_UI_ERROR:
        return "UI_ERROR";
    case JPP_APP_SESSION_STATUS_VM_ERROR:
        return "VM_ERROR";
    case JPP_APP_SESSION_STATUS_WATCHDOG_SUSPEND:
        return "WATCHDOG_SUSPEND";
    case JPP_APP_SESSION_STATUS_WATCHDOG_KILL:
        return "WATCHDOG_KILL";
    }
    return "UNKNOWN";
}

const char *jpp_app_failure_phase_name(jpp_app_failure_phase_t phase)
{
    switch (phase) {
    case JPP_APP_FAILURE_NONE:
        return "NONE";
    case JPP_APP_FAILURE_CREATE:
        return "CREATE";
    case JPP_APP_FAILURE_START:
        return "START";
    case JPP_APP_FAILURE_FOREGROUND:
        return "FOREGROUND";
    case JPP_APP_FAILURE_ACTION:
        return "ACTION";
    case JPP_APP_FAILURE_BACKGROUND:
        return "BACKGROUND";
    case JPP_APP_FAILURE_SD:
        return "SD";
    }
    return "UNKNOWN";
}

const char *jpp_app_failure_code_name(jpp_app_failure_code_t code)
{
    switch (code) {
    case JPP_APP_FAILURE_CODE_NONE:
        return "NONE";
    case JPP_APP_FAILURE_CODE_CREATE_EXCEPTION:
        return "APP_CREATE_EXCEPTION";
    case JPP_APP_FAILURE_CODE_START_EXCEPTION:
        return "APP_START_EXCEPTION";
    case JPP_APP_FAILURE_CODE_FOREGROUND_EXCEPTION:
        return "APP_FOREGROUND_EXCEPTION";
    case JPP_APP_FAILURE_CODE_ACTION_EXCEPTION:
        return "APP_ACTION_EXCEPTION";
    case JPP_APP_FAILURE_CODE_BACKGROUND_EXCEPTION:
        return "APP_BACKGROUND_EXCEPTION";
    case JPP_APP_FAILURE_CODE_VM_ERROR:
        return "APP_VM_ERROR";
    case JPP_APP_FAILURE_CODE_BACKGROUND_WATCHDOG:
        return "APP_BACKGROUND_WATCHDOG";
    case JPP_APP_FAILURE_CODE_SD_REMOVED:
        return "APP_SD_REMOVED";
    }
    return "UNKNOWN";
}
