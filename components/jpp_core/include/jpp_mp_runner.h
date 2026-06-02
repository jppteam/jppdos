#pragma once

#include "jpp_sdk_bridge.h"
#include "jpp_vm_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPP_MP_RUNNER_OK = 0,
    JPP_MP_RUNNER_LOAD_FAILED,    /* .mpy file could not be opened or parsed */
    JPP_MP_RUNNER_NO_FACTORY,     /* module has no create_app() callable */
    JPP_MP_RUNNER_HOOK_ERROR,     /* unhandled Python exception in a lifecycle hook */
} jpp_mp_runner_result_t;

/*
 * Run a MicroPython app to completion.
 *
 * sdk             - Bound SDK context (jpp_sdk_bind must have been called).
 * vm              - VM context in RUNNING state with import surface configured.
 * entry_mpy_path  - Absolute path to the compiled .mpy entry file.
 *
 * Blocks until the app calls sdk.request_close(), the main loop signals stop,
 * or an unrecoverable error occurs. Always calls on_stop() before returning.
 */
jpp_mp_runner_result_t jpp_mp_runner_run(
    jpp_sdk_context_t *sdk,
    jpp_vm_context_t  *vm,
    const char        *entry_mpy_path
);

const char *jpp_mp_runner_result_name(jpp_mp_runner_result_t result);

#ifdef __cplusplus
}
#endif
