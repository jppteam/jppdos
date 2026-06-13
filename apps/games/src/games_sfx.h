#pragma once

/* games_sfx — asynchronous, mute-aware buzzer wrappers for the games. */

#include <stddef.h>
#include <stdint.h>

#include "jpp_sdk_bridge.h"

void games_sfx_init(jpp_sdk_context_t *ctx, bool enabled);
void games_sfx_set_enabled(bool enabled);
void games_sfx_tone(uint32_t freq_hz, uint32_t duration_ms);
void games_sfx_seq(const jpp_buzzer_note_t *notes, size_t count);
