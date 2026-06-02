#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpp_sdk_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * jpp_ble_native — NimBLE host stack integration for the JPP SDK bridge.
 *
 * Call jpp_ble_native_init() once at boot (after NVS is initialised, before
 * app discovery) to start the NimBLE host task and pre-register the generic
 * app-host GATT service (see JPP_SDK_BLE_HOST_*_UUID).
 *
 * Then call jpp_ble_native_get_services() and jpp_ble_native_get_host_services()
 * to obtain function pointers that are installed into jpp_sdk_native_services_t
 * before binding an app. Apps reach all of this through the App SDK
 * (jpp_sdk_ble_*); the host buffer API below is the firmware-internal backing
 * for the ble.host SDK calls and is not called by apps directly.
 */

/* GATT host service buffer sizes. */
#define JPP_BLE_HOST_TX_BUF_SIZE 2048u
#define JPP_BLE_HOST_RX_BUF_SIZE 4096u

/* Initialise the NimBLE stack.  Call once after nvs_flash_init(). */
void jpp_ble_native_init(void);

/*
 * Suspend the NimBLE stack: stop the host task and de-init the controller,
 * releasing its heap so WiFi has the headroom it needs during heavy networking
 * (e.g. a WebDAV transfer — see jpp_fileserver_core).  No-op (returns false) if
 * BLE is disabled at build time or already suspended.  Must NOT be called while
 * an app is actively using BLE; WebDAV and SD apps are mutually exclusive, so
 * the firmware only suspends BLE while no app is running.
 *
 * Returns true if the stack was running and is now suspended.
 */
bool jpp_ble_native_suspend(void);

/*
 * Resume a previously suspended NimBLE stack (re-init controller, re-register
 * the host GATT service, restart the host task).  No-op (returns false) if BLE
 * is disabled at build time or already running.
 *
 * Returns true if the stack was suspended and is now running.
 */
bool jpp_ble_native_resume(void);

/*
 * Fill all BLE-related function-pointer / context slots for
 * jpp_sdk_native_services_t.  Every output pointer is guaranteed non-NULL.
 */
void jpp_ble_native_get_services(
    jpp_sdk_ble_scan_fn_t            *out_scan,       void **out_scan_ctx,
    jpp_sdk_ble_advertise_start_fn_t *out_adv_start,
    jpp_sdk_ble_advertise_stop_fn_t  *out_adv_stop,   void **out_adv_ctx,
    jpp_sdk_ble_connect_fn_t         *out_connect,
    jpp_sdk_ble_read_char_fn_t       *out_read_char,
    jpp_sdk_ble_write_char_fn_t      *out_write_char,
    jpp_sdk_ble_disconnect_fn_t      *out_disconnect,  void **out_client_ctx,
    jpp_sdk_ble_service_register_fn_t   *out_svc_reg,
    jpp_sdk_ble_service_unregister_fn_t *out_svc_unreg, void **out_host_ctx
);

/*
 * Fill the GATT-host byte-exchange / connectable function-pointer slots for
 * jpp_sdk_native_services_t. Every output pointer is guaranteed non-NULL when
 * BT is enabled (NULL stubs otherwise).
 */
void jpp_ble_native_get_host_services(
    jpp_sdk_ble_host_set_value_fn_t  *out_set_value,
    jpp_sdk_ble_host_wait_write_fn_t *out_wait_write,
    jpp_sdk_ble_host_clear_fn_t      *out_clear,
    jpp_sdk_ble_set_connectable_fn_t *out_set_connectable
);

/*
 * Set whether subsequent jpp_sdk_ble_advertise_start() calls should use
 * connectable (UND) or non-connectable advertising mode.  Default: false.
 */
void jpp_ble_native_set_connectable(bool connectable);

/*
 * Update the TX characteristic buffer (participant → leader).
 * Thread-safe; callable from any FreeRTOS task.
 */
void jpp_ble_host_set_tx(const uint8_t *data, size_t len);

/*
 * Block until the RX characteristic has been written by a connected leader,
 * or until timeout_ms elapses.  Pass timeout_ms = 0 for an infinite wait.
 * On success, copies up to *len_out bytes into buf and updates *len_out.
 * Returns true if data arrived, false on timeout.
 */
bool jpp_ble_host_wait_rx(uint8_t *buf, size_t *len_out, uint32_t timeout_ms);

/* Discard any pending RX semaphore token and clear the RX buffer. */
void jpp_ble_host_clear_rx(void);

#ifdef __cplusplus
}
#endif
