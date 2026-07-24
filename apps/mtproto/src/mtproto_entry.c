/*
 * mtproto_entry — standard native-app entry point.
 *
 * The firmware's native loader finds and calls jpp_app_entry(ctx) by name.
 * No background tasks, so no jpp_app_task_entry is exported.
 */

#include "jpp_sdk_bridge.h"
#include "mtproto.h"

void jpp_app_entry(jpp_sdk_context_t *ctx)
{
    mtproto_run(ctx);
}
