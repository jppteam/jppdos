#pragma once

#include "jpp_sdk_bridge.h"
#include "jpp_broker_core.h"
#include "jpp_rtc_core.h"
#include "jpp_battery_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Global native services struct populated once during boot and reused for
 * every app session.  Call jpp_native_services_init() after BLE and RTC
 * state are ready.
 */
extern jpp_sdk_native_services_t s_native_services;
/* Service callbacks added after SDK v1 — installed with jpp_sdk_set_services_v2()
   after each bind, kept separate so s_native_services stays ABI-frozen. */
extern jpp_sdk_services_v2_t     s_native_services_v2;
extern jpp_broker_lock_set_t     s_broker_locks;

void jpp_native_services_init(jpp_rtc_state_t *rtc_state);

/* Point the device.status broker callback at the live battery state. The main
   loop owns the state and refreshes it; pass its stable address once at startup. */
void jpp_native_services_set_battery_state(jpp_battery_state_t *state);

/* Set to true by file I/O callbacks when an SD access error is detected.
   Checked by the main loop to show the SD ejection fatal screen. */
extern volatile bool s_sd_ejection_detected;

#ifdef __cplusplus
}
#endif
