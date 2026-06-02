/*
 * testapp_native_entry — standard native-app entry point.
 *
 * The firmware's native loader finds and calls jpp_app_entry(ctx) by name.
 * This translation unit provides that symbol and forwards to
 * testapp_native_run() so the rest of the app code does not need to know the
 * loader's naming contract.
 */

#include "jpp_sdk_bridge.h"
#include "testapp_native.h"

void jpp_app_entry(jpp_sdk_context_t *ctx)
{
    testapp_native_run(ctx);
}
