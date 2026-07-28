/*
 * testapp_native_entry — standard native-app entry points.
 *
 * The firmware's native loader finds and calls jpp_app_entry(ctx) by name;
 * jpp_app_task_entry(ctx, name) is the optional headless entry used for
 * scheduled background tasks. This translation unit provides both symbols
 * and forwards to the app code so the rest of it does not need to know the
 * loader's naming contract.
 */

#include "jpp_sdk_bridge.h"
#include "testapp_native.h"

void jpp_app_entry(jpp_sdk_context_t *ctx)
{
    testapp_native_run(ctx);
}

void jpp_app_task_entry(jpp_sdk_context_t *ctx, const char *task_name)
{
    testapp_native_run_task(ctx, task_name);
}
