#pragma once

#include "jpp_sdk_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * meetapp_run — entry point for the MeetApp native application.
 *
 * Called from a dedicated FreeRTOS task by app_main.  The function runs the
 * full meetup protocol and returns when the session is complete or the user
 * backs out.  The caller must call jpp_sdk_request_close() on ctx or the
 * app_main teardown path will handle it via ctx->close_requested.
 */
void meetapp_run(jpp_sdk_context_t *ctx);

#ifdef __cplusplus
}
#endif
