/*
 * Built-in native app registry (apps layer).
 *
 * The firmware discovers and launches native apps generically by iterating this
 * table (see jpp_native_app.h); it never references a particular app by name.
 * Each additional built-in native app adds an entry here with its launcher entry
 * point and declared capabilities.
 */

#include "jpp_native_app.h"

#include "meetapp.h"

/* Capabilities MeetApp declares. Tier-0 (files.scoped, device.status) are
   auto-granted; tier-1 (ble.scan, ble.advertise) are prompted once on first use
   and the grant persisted; tier-2 (ble.connect, ble.host) are prompted on first
   use each launch. BLE prompts fire only when the relevant flow is entered. */
static const char *const MEETAPP_CAPS[] = {
    "files.scoped",
    "device.status",
    "ble.scan",
    "ble.advertise",
    "ble.connect",
    "ble.host",
};

static const jpp_native_app_desc_t NATIVE_APPS[] = {
    {
        .app_id           = "meetapp",
        .name             = "MeetApp",
        .icon             = NULL,
        .entry            = meetapp_run,
        .capabilities     = MEETAPP_CAPS,
        .capability_count = sizeof(MEETAPP_CAPS) / sizeof(MEETAPP_CAPS[0]),
    },
};

const jpp_native_app_desc_t *jpp_native_app_registry(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(NATIVE_APPS) / sizeof(NATIVE_APPS[0]);
    }
    return NATIVE_APPS;
}
