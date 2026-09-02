#pragma once

/*
 * jpp_onboarding — first-boot welcome flow.
 *
 * Runs once (gated on an NVS flag), blocking, before the launcher UI takes
 * over: a welcome dialog (with unit serial/run count if LRV data is present),
 * an optional username prompt, then a greeting with a "Connect to Wi-Fi now?"
 * choice that hands off into Settings on Yes.
 *
 * Both title screens carry a status line on the last display row: the current
 * time on the left, the battery percentage on the right.
 */

#include "jpp_rtc_core.h"
#include "jpp_ui_core.h"
#include "jpp_settings_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * No-ops immediately if onboarding has already run (NVS jpp_onboard/done).
 *
 * `rtc` feeds the status line's clock and may be NULL (or hold no time yet),
 * in which case it renders "--:--".  The battery percentage comes from the
 * shell status the caller has already seeded (-1 = unknown, rendered blank) —
 * the main loop's own battery polling does not run until onboarding returns.
 */
void jpp_onboarding_run(jpp_ui_shell_t *shell, jpp_settings_state_t *settings_state,
                        const jpp_rtc_state_t *rtc);

#ifdef __cplusplus
}
#endif
