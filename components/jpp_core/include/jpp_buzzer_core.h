#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPP_BUZZER_STATUS_OK = 0,
    JPP_BUZZER_STATUS_INVALID_ARGUMENT,
    JPP_BUZZER_STATUS_NOT_INITIALIZED,
} jpp_buzzer_status_t;

/* Predefined named sounds. */
typedef enum {
    JPP_BUZZER_SOUND_SUCCESS = 0,
    JPP_BUZZER_SOUND_FAILURE,
    JPP_BUZZER_SOUND_NOTIFY,
    JPP_BUZZER_SOUND_STARTUP,
    JPP_BUZZER_SOUND_CLICK,
    JPP_BUZZER_SOUND_COUNT,
} jpp_buzzer_sound_t;

/* Selectable startup jingles (persisted in NVS jpp_sound/startup_jingle). */
typedef enum {
    JPP_STARTUP_JINGLE_DEFAULT    = 0, /* JPPDOS default chime */
    JPP_STARTUP_JINGLE_WINXP,          /* Windows XP Startup */
    JPP_STARTUP_JINGLE_WIN31,          /* Windows 3.1 Ta-da */
    JPP_STARTUP_JINGLE_MAC,            /* Macintosh 128K startup beep */
    JPP_STARTUP_JINGLE_RICKROLL,       /* Never Gonna Give You Up */
    JPP_STARTUP_JINGLE_NOKIA_ON,       /* Nokia Power On */
    JPP_STARTUP_JINGLE_NOKIA_TUNE,     /* Nokia Tune */
    JPP_STARTUP_JINGLE_SANDSTORM,      /* Darude — Sandstorm */
    JPP_STARTUP_JINGLE_DOOM,           /* DOOM E1M1 */
    JPP_STARTUP_JINGLE_CLUTTERFUNK,    /* Clutterfunk (Geometry Dash) */
    JPP_STARTUP_JINGLE_OFF,            /* no startup sound */
    JPP_STARTUP_JINGLE_COUNT,
} jpp_startup_jingle_t;

/* One note in a sequence (freq_hz==0 means silence / rest). */
typedef struct {
    uint32_t freq_hz;
    uint32_t duration_ms;
} jpp_buzzer_note_t;

/* Initialise LEDC and configure GPIO3 with boosted drive strength.
   Must be called once before any other buzzer function. */
jpp_buzzer_status_t jpp_buzzer_init(void);

/* Set buzzer volume as a percentage (0 = mute, 100 = full duty / max volume).
   Values above 100 are clamped to 100.  Default after init is 100.
   May be called before or after jpp_buzzer_init; takes effect on the next tone. */
jpp_buzzer_status_t jpp_buzzer_set_volume(uint8_t percent);

/* Return the current volume percentage (0–100). */
uint8_t jpp_buzzer_get_volume(void);

/* Play a single tone, blocking the caller for duration_ms, then silence. */
jpp_buzzer_status_t jpp_buzzer_tone(uint32_t freq_hz, uint32_t duration_ms);

/* Stop the buzzer immediately.  Also cancels any async sequence in flight. */
jpp_buzzer_status_t jpp_buzzer_stop(void);

/* Play a predefined named sound sequence (blocking). */
jpp_buzzer_status_t jpp_buzzer_play(jpp_buzzer_sound_t sound);

/* Play a caller-supplied sequence of notes (blocking). */
jpp_buzzer_status_t jpp_buzzer_play_sequence(const jpp_buzzer_note_t *notes,
                                              size_t count);

const char *jpp_buzzer_status_name(jpp_buzzer_status_t status);
const char *jpp_buzzer_sound_name(jpp_buzzer_sound_t sound);

/* Play the selected startup jingle (JPP_STARTUP_JINGLE_OFF = no-op, blocking). */
jpp_buzzer_status_t jpp_buzzer_play_startup_jingle(jpp_startup_jingle_t jingle);

/*
 * Async (non-blocking) playback.  These return immediately; a dedicated player
 * task drives the output.  Submitting a new sequence — or calling
 * jpp_buzzer_stop() — preempts whatever is currently playing, so cycling
 * previews cuts the previous one off within one note.  The notes are copied
 * into an internal buffer (up to 64 entries), so the caller's array need not
 * outlive the call.  If the player task is unavailable they fall back to
 * blocking playback.
 */
jpp_buzzer_status_t jpp_buzzer_play_sequence_async(const jpp_buzzer_note_t *notes,
                                                   size_t count);
jpp_buzzer_status_t jpp_buzzer_play_async(jpp_buzzer_sound_t sound);
jpp_buzzer_status_t jpp_buzzer_play_startup_jingle_async(jpp_startup_jingle_t jingle);

/* Short display name for a startup jingle (fits in ~16 chars). */
const char *jpp_startup_jingle_name(jpp_startup_jingle_t jingle);

#ifdef __cplusplus
}
#endif
