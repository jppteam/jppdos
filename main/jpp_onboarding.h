#pragma once

/*
 * jpp_onboarding — first-boot welcome flow.
 *
 * Runs once (gated on an NVS flag), blocking, before the launcher UI takes
 * over: a welcome dialog (with unit serial/run count if LRV data is unlocked),
 * an optional username prompt, then a greeting with a "Connect to Wi-Fi now?"
 * choice that hands off into Settings on Yes.
 */

#include "jpp_ui_core.h"
#include "jpp_settings_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* No-ops immediately if onboarding has already run (NVS jpp_onboard/done). */
void jpp_onboarding_run(jpp_ui_shell_t *shell, jpp_settings_state_t *settings_state);

#ifdef __cplusplus
}
#endif
