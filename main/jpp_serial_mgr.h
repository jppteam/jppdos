#pragma once

/*
 * J++Device Serial Management Protocol (JPPD-SMP) — firmware side.
 *
 * Runs a background UART-RX task on UART_NUM_0 (same port as ESP_LOG text
 * output).  Binary frames are framed with a 4-byte SOF magic that never
 * appears in ESP_LOG output; a TX mutex prevents frame bytes from
 * interleaving with log lines.
 *
 * Lives in main/ so it can use ssd1306.h, jpp_app_dispatch.h, and
 * jpp_settings_screen.h without creating circular component dependencies.
 */

#include <stdbool.h>
#include "jpp_ui_core.h"   /* jpp_ui_action_t */
#include "jpp_rtc_core.h"  /* jpp_rtc_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Call from app_main after UART0 is available (after nvs / BLE init, step 6). */
void jpp_serial_mgr_init(void);

/* Provide an RTC state pointer for challenge timestamp generation (may be NULL). */
void jpp_serial_mgr_set_rtc(jpp_rtc_state_t *rtc);

/* Call from the main-loop action drain for every queued keypad action. */
void jpp_serial_mgr_handle_action(jpp_ui_action_t action);

/* Call from the main-loop render path each tick when needs_render() is true. */
void jpp_serial_mgr_render(void);

/* True while the consent dialog or active-session screen owns the display. */
bool jpp_serial_mgr_needs_render(void);

/* True while a session is active; used to block SD app launch. */
bool jpp_serial_mgr_session_active(void);

#ifdef __cplusplus
}
#endif
