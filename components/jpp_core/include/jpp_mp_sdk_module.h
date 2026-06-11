#pragma once

#include "jpp_sdk_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Set the SDK context that the jppsdk Python module will operate on.
 * Must be called before any Python code runs. The pointer must remain
 * valid for the lifetime of the MicroPython interpreter session.
 */
void jpp_mp_sdk_module_set_context(jpp_sdk_context_t *ctx);

#ifdef __cplusplus
}
#endif
