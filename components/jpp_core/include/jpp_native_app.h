#pragma once

/*
 * jpp_native_app.h — legacy native-app registry bridge (retired).
 *
 * The registry bridge was a temporary workaround while the native ELF loader
 * was pending: native app binaries were statically linked into the firmware
 * image and dispatched through a compile-time table. That approach required
 * firmware changes to add or update any native app.
 *
 * The dynamic loader (jpp_native_loader_core) supersedes it. Native apps are
 * now regular manifest-described packages on the SD card:
 *
 *   /sd/apps/<app_id>/
 *     manifest.json        app_type: "native", entry: "<app_id>.bin"
 *     <app_id>.bin         ELF32 RISC-V shared object (jpp_app_entry export)
 *
 * The firmware discovers them by SD scan at boot (same as MicroPython apps),
 * loads the ELF at launch using jpp_native_loader_core, and frees the memory
 * when the app exits. No firmware rebuild is needed to add or update apps.
 *
 * This header is retained for reference and backward compatibility with any
 * out-of-tree code that still references jpp_native_app_entry_fn. New code
 * should use jpp_native_loader_core.h instead.
 */

#include <stddef.h>
#include <stdint.h>

#include "jpp_sdk_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Entry-function signature for native apps — used by both the old registry
 * bridge and the ELF loader (jpp_app_entry has this signature).
 */
typedef void (*jpp_native_app_entry_fn)(jpp_sdk_context_t *context);

#ifdef __cplusplus
}
#endif
