#pragma once

/*
 * jpp_native_loader_core — ELF32/RISC-V position-independent app loader.
 *
 * Loads a native app binary (ELF32 shared object, app_type "native") from the
 * SD card, resolves its external symbols against the firmware's exported symbol
 * table, and calls its jpp_app_entry(jpp_sdk_context_t *) entry point.
 *
 * An app binary is produced by compiling its sources with:
 *   -fPIC -shared -mno-relax -nostartfiles
 * targeting riscv32-esp-elf and exporting a single global function named
 * jpp_app_entry.  The firmware SDK headers (jpp_sdk_bridge.h, etc.) are used
 * at compile time; all SDK symbols are resolved at load time by this module.
 *
 * Usage:
 *   jpp_native_loaded_app_t *app = NULL;
 *   jpp_native_loader_result_t r = jpp_native_loader_load(path, &app);
 *   if (r == JPP_NATIVE_LOADER_OK) {
 *       jpp_native_loader_run(app, ctx);
 *       jpp_native_loader_free(app);
 *   }
 */

#include <stddef.h>
#include <stdint.h>
#include "jpp_sdk_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPP_NATIVE_LOADER_OK = 0,
    JPP_NATIVE_LOADER_READ_FAILED,      /* fopen/fread error                  */
    JPP_NATIVE_LOADER_INVALID_ELF,      /* not a valid ELF32/RISC-V shared obj */
    JPP_NATIVE_LOADER_UNSUPPORTED,      /* relocation type or ABI not handled  */
    JPP_NATIVE_LOADER_NO_MEMORY,        /* heap allocation failed              */
    JPP_NATIVE_LOADER_UNRESOLVED_SYM,   /* symbol not in firmware export table */
    JPP_NATIVE_LOADER_NO_ENTRY,         /* jpp_app_entry not found in binary   */
} jpp_native_loader_result_t;

/* Opaque handle to a loaded (but not yet run) native app. */
typedef struct jpp_native_loaded_app jpp_native_loaded_app_t;

/*
 * Optional boot-time call that logs the executable app-pool capacity.
 * Native code loads into the shared jpp_app_pool — a static .bss buffer
 * (always contiguous, executable because CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n
 * maps SRAM RWX); no heap allocation is performed.
 */
void jpp_native_loader_preinit(void);

/*
 * Load a native app binary from `path` (absolute path to the .bin ELF).
 * On success *out_app is filled and JPP_NATIVE_LOADER_OK is returned.
 * The caller must call jpp_native_loader_free() when finished.
 */
jpp_native_loader_result_t jpp_native_loader_load(
    const char                  *path,
    jpp_native_loaded_app_t    **out_app
);

/*
 * Call the loaded app's jpp_app_entry(ctx) and block until it returns.
 * The app must have been successfully loaded with jpp_native_loader_load().
 */
void jpp_native_loader_run(jpp_native_loaded_app_t *app,
                            jpp_sdk_context_t       *ctx);

/*
 * Call the loaded app's jpp_app_task_entry(ctx, task_name) — the optional
 * headless background-task entry point — and block until it returns.
 * Returns false (without calling anything) when the binary does not export
 * jpp_app_task_entry.
 */
bool jpp_native_loader_run_task(jpp_native_loaded_app_t *app,
                                jpp_sdk_context_t       *ctx,
                                const char              *task_name);

/*
 * Release all memory owned by the loaded app handle.
 * Safe to call with app == NULL.
 */
void jpp_native_loader_free(jpp_native_loaded_app_t *app);

const char *jpp_native_loader_result_name(jpp_native_loader_result_t r);

#ifdef __cplusplus
}
#endif
