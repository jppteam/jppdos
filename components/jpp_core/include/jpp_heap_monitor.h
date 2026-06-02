#pragma once

/*
 * jpp_heap_monitor — global heap pressure diagnostics.
 *
 * The ESP32-C6 has a single unified SRAM (no PSRAM): the WiFi driver, lwIP,
 * BLE controller, the native-app exec pool, and every malloc all draw from the
 * same heap.  When free heap drops too low, the WiFi driver silently fails to
 * allocate management/data frames and the radio appears to "lose signal" — a
 * recurring, hard-to-attribute failure mode on this firmware.
 *
 * This module makes that pressure visible:
 *
 *   1. A failed-allocation callback (heap_caps_register_failed_alloc_callback)
 *      logs the *exact* size, caps, and calling function of any malloc that
 *      fails — turning a cryptic "wifi:m f ..." into an attributable OOM line.
 *
 *   2. A low-priority background task samples free heap on an interval and logs
 *      a WARN when it crosses below JPP_HEAP_MON_WARN_BYTES and an ERROR below
 *      JPP_HEAP_MON_CRIT_BYTES, with hysteresis so it does not spam.  It also
 *      tracks and reports the all-time minimum (low-water mark).
 *
 * Call jpp_heap_monitor_init() once early in boot (after the heap is up, i.e.
 * any time in app_main).  Use jpp_heap_monitor_log() for manual checkpoints
 * around suspected heavy operations.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sampling interval for the background monitor task (milliseconds). */
#ifndef JPP_HEAP_MON_INTERVAL_MS
#define JPP_HEAP_MON_INTERVAL_MS 5000u
#endif

/* Free-heap thresholds.  WARN ≈ "WiFi getting tight"; CRIT ≈ "frame allocs
   about to fail".  Tuned for the C6 unified-SRAM heap budget. */
#ifndef JPP_HEAP_MON_WARN_BYTES
#define JPP_HEAP_MON_WARN_BYTES (30u * 1024u)
#endif
#ifndef JPP_HEAP_MON_CRIT_BYTES
#define JPP_HEAP_MON_CRIT_BYTES (15u * 1024u)
#endif

/*
 * Register the failed-alloc callback and start the background monitor task.
 * Idempotent: a second call is a no-op.  Returns true on success.
 */
bool jpp_heap_monitor_init(void);

/*
 * Emit one heap line immediately: "heap @<label>: free=.. min=.. largest=..".
 * Safe to call from any task.  `label` is a short context string (e.g. "webdav
 * start", "app launch").
 */
void jpp_heap_monitor_log(const char *label);

#ifdef __cplusplus
}
#endif
