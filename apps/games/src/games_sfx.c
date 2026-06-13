#include "games_sfx.h"

static jpp_sdk_context_t *s_ctx;
static bool s_enabled = true;

void games_sfx_init(jpp_sdk_context_t *ctx, bool enabled)
{
    s_ctx = ctx;
    s_enabled = enabled;
}

void games_sfx_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

void games_sfx_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    /* One-note async sequence so the game loop never blocks on a tone. */
    jpp_buzzer_note_t note = { .freq_hz = freq_hz, .duration_ms = duration_ms };
    games_sfx_seq(&note, 1u);
}

void games_sfx_seq(const jpp_buzzer_note_t *notes, size_t count)
{
    if (s_ctx == NULL || !s_enabled) {
        return;
    }
    jpp_sdk_buzzer_play_sequence_async(s_ctx, notes, count);
}
