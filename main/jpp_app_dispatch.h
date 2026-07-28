#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "jpp_boot_core.h"
#include "jpp_ui_core.h"
#include "jpp_sdk_bridge.h"
#include "jpp_vm_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared app-runtime state.  Defined in jpp_app_dispatch.c; accessed by
 * app_main.c (keypad task and UI loop) and jpp_native_services.c (path prompt).
 */
/* Action queue shared with the keypad task (defined in app_main.c). */
extern QueueHandle_t       s_action_queue;

extern jpp_sdk_context_t  *s_active_sdk_context;
extern jpp_sdk_context_t   s_sd_ctx;
extern jpp_vm_context_t    s_sd_vm;
extern TaskHandle_t        s_sd_task;
extern bool                s_sd_is_mp;

/*
 * discover_apps — scan the SD card for installed apps and populate the shell
 * launcher.  Normal mode includes the SD scan; recovery mode shows only
 * built-in system entries.
 */
void discover_apps(bool normal_mode,
                   jpp_boot_discovery_summary_t *summary,
                   jpp_ui_shell_t *shell);

/*
 * launch_sd_app — load and start an app from /sd/apps/<app_id>/.
 * Handles both MicroPython (app_type "micropython") and native binary
 * (app_type "native") apps via their respective runtime paths.
 * Returns false if the manifest is missing, invalid, or fails preflight.
 */
bool launch_sd_app(const char *app_id, const jpp_boot_context_t *boot);

/*
 * Background app re-discovery — non-blocking SD rescan for the launcher.
 * Call discover_apps_background_start() once; poll
 * discover_apps_background_ready() each loop tick; when it returns true
 * call discover_apps_apply_to_shell() to update the shell's app list.
 */
void discover_apps_background_start(bool normal_mode);
bool discover_apps_background_ready(void);
void discover_apps_apply_to_shell(jpp_ui_shell_t *shell);

void teardown_sd_app(jpp_ui_shell_t *shell);

/*
 * jpp_app_crash_take — one-shot crash report consumer for the main loop.
 * When the app task ends in a failure (MicroPython load/hook error or native
 * load failure) the dispatcher records the APP_CRASH marker, appends a line to
 * /data/ui_crash.log, and arms this flag.  The main loop calls this after
 * teardown; on true it copies the crashed app id and reason (for the crash
 * dialog) and clears the flag.
 */
bool jpp_app_crash_take(char *app_id, size_t app_id_len,
                        char *reason, size_t reason_len);

/*
 * Headless background-task runs (driven by jpp_bg_scheduler from the main
 * loop). jpp_app_bg_launch starts the app's on_task(name) /
 * jpp_app_task_entry run on the shared app task slot; the main loop polls
 * jpp_app_bg_finished() and calls jpp_app_bg_teardown() (also used to kill an
 * over-quota run before esp_restart). Consent prompts are denied while a
 * headless run is active.
 */
bool jpp_app_bg_launch(const char *app_id, const char *task_name,
                       const jpp_boot_context_t *boot);
bool jpp_app_bg_running(void);
bool jpp_app_bg_finished(void);
void jpp_app_bg_teardown(void);

/*
 * jpp_app_consent_prompt — show an interactive permission dialog for `cap` and
 * (for tier-1 caps) persist the grant.  Called from the consent_prompt_cb in
 * jpp_native_services when jpp_sdk_ensure_cap triggers a lazy prompt.
 */
bool jpp_app_consent_prompt(jpp_sdk_context_t *sdk_ctx, const char *cap, int tier);

/*
 * jpp_app_origin_prompt — per-origin consent for https.request.  Returns true
 * immediately for an origin already persisted in /data/grants/<app_id>.origins,
 * otherwise shows an interactive dialog naming the origin and persists an
 * allow.  Called from the origin_prompt callback in jpp_native_services when
 * jpp_sdk_https_request() is about to issue a request.
 */
bool jpp_app_origin_prompt(jpp_sdk_context_t *sdk_ctx, const char *origin);

#ifdef JPP_WOKWI_EMBED_APP
bool launch_wokwi_embed_app(const char *app_id, const jpp_boot_context_t *boot);
#endif

#ifdef __cplusplus
}
#endif
