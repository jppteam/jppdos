#pragma once

#include <stddef.h>
#include <stdint.h>

#include "jpp_sdk_bridge.h"  /* jpp_sdk_espnow_send_fn_t / jpp_sdk_espnow_recv_fn_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fill the ESP-NOW service function pointers for the SDK bridge.  Mirrors
 * jpp_ble_native_get_services(): jpp_core cannot depend on main/, so this
 * module brings up the WiFi driver (STA mode, idempotent) and esp_now itself
 * on first use rather than relying on main/jpp_wifi_init.c.
 */
void jpp_espnow_native_get_services(jpp_sdk_espnow_send_fn_t *out_send,
                                     jpp_sdk_espnow_recv_fn_t *out_recv,
                                     void **out_context);

#ifdef __cplusplus
}
#endif
