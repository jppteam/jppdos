#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * jpp_app_pool — single shared static memory pool for the running app.
 *
 * Native apps need a contiguous, executable (RWX) block to load their code
 * into; MicroPython apps need a contiguous block for their GC heap.  Both
 * requirements are satisfied by one static `.bss` buffer:
 *   - contiguous: placed by the linker, never fragmented by the allocator
 *   - executable: with CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n the whole SRAM
 *     range is mapped RWX, so .bss is executable on ESP32-C6
 *   - reservation-free at runtime: it is BSS, not a heap allocation, so a
 *     fragmented post-WiFi/BLE heap cannot starve an app launch
 *
 * Crucially, native and MicroPython apps are mutually exclusive — only one SD
 * app task runs at a time — so a single pool serves both.  Merging the former
 * separate 64 KB exec pool and 32 KB GC pool into one 64 KB pool returns ~32 KB
 * of SRAM to the general heap, which on this single-SRAM part is the same heap
 * WiFi/lwIP draw frame buffers from.
 *
 * Sizing: 64 KB fits the largest native app (MeetApp, ~50 KB loaded) with
 * headroom, and is far above the typical MicroPython GC footprint (~15-20 KB).
 * Raise JPP_APP_POOL_BYTES (and rebuild) if a future app needs more.
 */

#define JPP_APP_POOL_BYTES (64u * 1024u)

/*
 * Acquire the shared pool.  Returns the pool base pointer and (if out_size is
 * non-NULL) its capacity, marking it in-use.  Returns NULL if the pool is
 * already held or if need_bytes exceeds the pool capacity.  Pass need_bytes = 0
 * to acquire the whole pool regardless of size (used by the GC heap).
 */
void  *jpp_app_pool_acquire(size_t need_bytes, size_t *out_size);

/* Release the pool back to the free state.  No-op if not currently held. */
void   jpp_app_pool_release(void);

/* Pool capacity in bytes (compile-time constant, exposed for logging). */
size_t jpp_app_pool_size(void);

/* True while the pool is held by an app. */
bool   jpp_app_pool_in_use(void);

#ifdef __cplusplus
}
#endif
