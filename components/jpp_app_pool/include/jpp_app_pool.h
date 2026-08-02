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
 * app task runs at a time — so a single pool serves both.  Sharing one pool
 * instead of reserving separate exec and GC pools keeps the static footprint
 * minimal, which on this single-SRAM part matters: every reserved KB comes out
 * of the same heap WiFi/lwIP draw frame buffers from.
 *
 * The WebDAV and LRV verification servers are the third class of holder (see
 * jpp_http_server_core).  They are foreground activities, mutually exclusive
 * with apps for exactly the same reason, and they take their task stack and
 * their I/O buffers from here rather than from the heap — which is what keeps
 * a WebDAV transfer from starving the WiFi driver of frame buffers.  Unlike an
 * app image, a server wants several separate allocations, so it carves them
 * with jpp_app_pool_alloc() instead of taking the base pointer.
 *
 * Sizing: 64 KB fits the largest native app (MeetApp, ~50 KB loaded) with
 * headroom, and is far above the typical MicroPython GC footprint (~15-20 KB).
 * Raise JPP_APP_POOL_BYTES (and rebuild) if an app needs more.
 */

#define JPP_APP_POOL_BYTES (64u * 1024u)

/*
 * Acquire the shared pool for an owner (a short label such as "app" or
 * "webdav", used in logs and reported by jpp_app_pool_owner() — the pointer is
 * stored, not copied, so pass a string literal or other static).  Returns the
 * pool base pointer and (if out_size is non-NULL) its capacity, marking it
 * in-use.  Returns NULL if the pool is already held or if need_bytes exceeds
 * the pool capacity.  Pass need_bytes = 0 to acquire the whole pool regardless
 * of size (used by the GC heap).  The bump pointer used by
 * jpp_app_pool_alloc() is reset here.
 */
void  *jpp_app_pool_acquire_as(const char *owner, size_t need_bytes, size_t *out_size);

/* Equivalent to jpp_app_pool_acquire_as("app", ...). */
void  *jpp_app_pool_acquire(size_t need_bytes, size_t *out_size);

/*
 * Bump-allocate `bytes` from the pool, aligned up to `align` (rounded up to at
 * least 4, must be a power of two).  Only valid while the pool is held by the
 * caller; allocations are freed as a group by jpp_app_pool_release().  Returns
 * NULL when the pool is not held or the remaining space is too small.
 *
 * Holders that want the whole pool as one block (the native loader, the
 * MicroPython GC heap) use the acquire base pointer and ignore this.
 */
void  *jpp_app_pool_alloc(size_t bytes, size_t align);

/* Bytes still unallocated by jpp_app_pool_alloc(); pool size when not held. */
size_t jpp_app_pool_avail(void);

/* Release the pool back to the free state.  No-op if not currently held. */
void   jpp_app_pool_release(void);

/* Label of the current holder, or NULL when the pool is free. */
const char *jpp_app_pool_owner(void);

/* Pool capacity in bytes (compile-time constant, exposed for logging). */
size_t jpp_app_pool_size(void);

/* True while the pool is held by an app. */
bool   jpp_app_pool_in_use(void);

#ifdef __cplusplus
}
#endif
