/*
 * meetapp_entry — standard native-app entry point.
 *
 * The firmware's native loader finds and calls jpp_app_entry(ctx) by name.
 * This translation unit provides that symbol and forwards to meetapp_run so
 * the rest of the app code does not need to know the loader's naming contract.
 */

#include "jpp_sdk_bridge.h"
#include "meetapp.h"

void jpp_app_entry(jpp_sdk_context_t *ctx)
{
    meetapp_run(ctx);
}
