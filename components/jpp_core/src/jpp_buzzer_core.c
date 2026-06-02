#include "../include/jpp_buzzer_core.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* Pull in pin/channel constants from the board config header.
   jpp_core cannot directly include main/jpp_hw_config.h, so the relevant
   constants are duplicated here as defaults that match the header values. */
#ifndef JPP_HW_BUZZER_GPIO
#define JPP_HW_BUZZER_GPIO           3
#endif
#ifndef JPP_HW_BUZZER_LEDC_TIMER
#define JPP_HW_BUZZER_LEDC_TIMER     LEDC_TIMER_0
#endif
#ifndef JPP_HW_BUZZER_LEDC_CHANNEL
#define JPP_HW_BUZZER_LEDC_CHANNEL   LEDC_CHANNEL_0
#endif
#ifndef JPP_HW_BUZZER_LEDC_MODE
#define JPP_HW_BUZZER_LEDC_MODE      LEDC_LOW_SPEED_MODE
#endif
#ifndef JPP_HW_BUZZER_RESOLUTION
#define JPP_HW_BUZZER_RESOLUTION     LEDC_TIMER_10_BIT
#endif
#ifndef JPP_HW_BUZZER_DUTY
#define JPP_HW_BUZZER_DUTY           512u
#endif

static bool    s_initialized = false;
static uint8_t s_volume_pct  = 100u; /* 0 = mute, 100 = full duty */

/* ---- Predefined sound sequences ----------------------------------------- */

static const jpp_buzzer_note_t SOUND_SUCCESS[] = {
    {880, 80}, {1320, 120},
};
static const jpp_buzzer_note_t SOUND_FAILURE[] = {
    {440, 100}, {0, 30}, {330, 200},
};
static const jpp_buzzer_note_t SOUND_NOTIFY[] = {
    {660, 60}, {0, 20}, {880, 60},
};
static const jpp_buzzer_note_t SOUND_STARTUP[] = {
    {523, 80}, {659, 80}, {784, 80}, {1047, 150},
};
static const jpp_buzzer_note_t SOUND_CLICK[] = {
    {1200, 20},
};

typedef struct { const jpp_buzzer_note_t *notes; size_t count; } sound_entry_t;

static const sound_entry_t SOUNDS[JPP_BUZZER_SOUND_COUNT] = {
    [JPP_BUZZER_SOUND_SUCCESS] = {SOUND_SUCCESS, sizeof(SOUND_SUCCESS)/sizeof(SOUND_SUCCESS[0])},
    [JPP_BUZZER_SOUND_FAILURE] = {SOUND_FAILURE, sizeof(SOUND_FAILURE)/sizeof(SOUND_FAILURE[0])},
    [JPP_BUZZER_SOUND_NOTIFY]  = {SOUND_NOTIFY,  sizeof(SOUND_NOTIFY) /sizeof(SOUND_NOTIFY[0])},
    [JPP_BUZZER_SOUND_STARTUP] = {SOUND_STARTUP, sizeof(SOUND_STARTUP)/sizeof(SOUND_STARTUP[0])},
    [JPP_BUZZER_SOUND_CLICK]   = {SOUND_CLICK,   sizeof(SOUND_CLICK)  /sizeof(SOUND_CLICK[0])},
};

/* ---- Startup jingle sequences ------------------------------------------- */

/* Note frequency shorthands used only by jingle sequences below. */
#define J_E2   82u
#define J_AS2 117u
#define J_B2  123u
#define J_C3  131u
#define J_D3  147u
#define J_E3  165u
#define J_C4  262u
#define J_CS4 277u
#define J_D4  294u
#define J_F4  349u
#define J_G4  392u
#define J_DS4 311u
#define J_E4  330u
#define J_FS4 370u
#define J_GS4 415u
#define J_A4  440u
#define J_AS4 466u
#define J_B4  494u
#define J_C5  523u
#define J_CS5 554u
#define J_D5  587u
#define J_DS5 622u
#define J_E5  659u
#define J_F5  698u
#define J_FS5 740u
#define J_G5  784u
#define J_GS5 831u
#define J_A5  880u
#define J_AS5 932u
#define J_B5  988u
#define J_C6 1047u
#define J_CS6 1109u
#define J_D6 1175u
#define J_DS6 1245u
#define J_E6 1319u
#define J_F6 1397u
#define J_FS6 1480u
#define J_G6 1568u
#define J_GS6 1661u
#define J_AS6 1865u
#define J_DS7 2489u

/* Windows XP Startup (RTTTL d=8,o=7,b=100:
   d#,16d#6,a#6,g#6,16p,d#,4a#6). */
static const jpp_buzzer_note_t JINGLE_WINXP[] = {
    {J_DS7, 300}, {J_DS6, 150}, {J_AS6, 300}, {J_GS6, 300},
    {0, 150}, {J_DS7, 300}, {J_AS6, 600},
};

/* Windows 3.1 (RTTTL d=8,o=7,b=110: 16e5,16p,4e5). */
static const jpp_buzzer_note_t JINGLE_WIN31[] = {
    {J_E5, 136}, {0, 136}, {J_E5, 545},
};

/* Macintosh 128K startup beep (RTTTL d=2,o=5,b=180: d) — a single tone. */
static const jpp_buzzer_note_t JINGLE_MAC[] = {
    {J_D5, 667},
};

/* Never Gonna Give You Up — chorus hook (RTTTL d=4,o=4,b=350:
   b,d5,b,2f5#,2f5#,2e5,8p,b,d5,b,2e5,2e5,d5,c5#,b,8p).  Small rests carved
   out of each note so the repeated F#5/E5 pairs articulate as two notes. */
static const jpp_buzzer_note_t JINGLE_RICKROLL[] = {
    {J_B4, 156}, {0, 15}, {J_D5, 156}, {0, 15}, {J_B4, 156}, {0, 15},
    {J_FS5, 328}, {0, 15}, {J_FS5, 328}, {0, 15}, {J_E5, 328}, {0, 15},
    {0, 86},
    {J_B4, 156}, {0, 15}, {J_D5, 156}, {0, 15}, {J_B4, 156}, {0, 15},
    {J_E5, 328}, {0, 15}, {J_E5, 328}, {0, 15}, {J_D5, 156}, {0, 15},
    {J_CS5, 156}, {0, 15}, {J_B4, 156}, {0, 15},
    {0, 86},
};

/* Nokia Power On (RTTTL d=8,o=7,b=140: p,g#6,f#6,4a#5,4c#6,4f#.6). */
static const jpp_buzzer_note_t JINGLE_NOKIA_ON[] = {
    {0, 214}, {J_GS6, 214}, {J_FS6, 214},
    {J_AS5, 429}, {J_CS6, 429}, {J_FS6, 643},
};

/* Nokia Tune — classic ringtone first phrase */
static const jpp_buzzer_note_t JINGLE_NOKIA_TUNE[] = {
    {J_E5, 167}, {J_D5, 167}, {J_FS4, 333}, {J_GS4, 333},
    {J_CS5, 167}, {J_B4, 167}, {J_D4,  333}, {J_E4,  333},
    {J_B4, 167}, {J_A4, 167}, {J_CS4, 333}, {J_E4,  333},
    {J_A4, 667},
};

/* Darude Sandstorm (RTTTL d=16,o=6,b=112:
   d,d,d,d,8d,d,d,d,d,d,d,8d,g,g,g,g,g,g,8g,f,f,f,f,f,f,8f).  The 16th-note
   stutter only works if each repeated note re-attacks, so a short rest is
   carved out of every note (116+18≈134 ms 16th, 250+18≈268 ms eighth). */
#define SS_D  {J_D6, 116}, {0, 18}
#define SS_D8 {J_D6, 250}, {0, 18}
#define SS_G  {J_G6, 116}, {0, 18}
#define SS_G8 {J_G6, 250}, {0, 18}
#define SS_F  {J_F6, 116}, {0, 18}
#define SS_F8 {J_F6, 250}, {0, 18}
static const jpp_buzzer_note_t JINGLE_SANDSTORM[] = {
    SS_D, SS_D, SS_D, SS_D, SS_D8,
    SS_D, SS_D, SS_D, SS_D, SS_D, SS_D, SS_D8,
    SS_G, SS_G, SS_G, SS_G, SS_G, SS_G, SS_G8,
    SS_F, SS_F, SS_F, SS_F, SS_F, SS_F, SS_F8,
};
#undef SS_D
#undef SS_D8
#undef SS_G
#undef SS_G8
#undef SS_F
#undef SS_F8

/* DOOM E1M1 "At Doom's Gate" — first riff (robsoncouto/arduino-songs, tempo
   225 → eighth ≈ 133 ms): E2 ostinato punctuated by E3, D3, C3, resolving on a
   long A#2.  Short rests separate the repeated low E2 pulses. */
#define DM_E2 {J_E2, 115}, {0, 18}
static const jpp_buzzer_note_t JINGLE_DOOM[] = {
    DM_E2, DM_E2, {J_E3, 133}, DM_E2, DM_E2, {J_D3, 133},
    DM_E2, DM_E2, {J_C3, 133}, DM_E2, DM_E2, {J_AS2, 800},
};
#undef DM_E2

/* Clutterfunk (RTTTL d=8,o=7,b=160:
   16c4,p.,16f#4,p.,16f4,p.,16f#4,p.,16g4,p,16f4,16f#4,16p,16g4,16p,16f#4). */
static const jpp_buzzer_note_t JINGLE_CLUTTERFUNK[] = {
    {J_C4, 94}, {0, 281}, {J_FS4, 94}, {0, 281},
    {J_F4, 94}, {0, 281}, {J_FS4, 94}, {0, 281},
    {J_G4, 94}, {0, 188}, {J_F4, 94}, {J_FS4, 94},
    {0, 94}, {J_G4, 94}, {0, 94}, {J_FS4, 94},
};

typedef struct { const jpp_buzzer_note_t *notes; size_t count; } jingle_entry_t;

#define JINGLE_ENTRY(arr) {(arr), sizeof(arr)/sizeof((arr)[0])}

static const jingle_entry_t JINGLES[JPP_STARTUP_JINGLE_COUNT] = {
    [JPP_STARTUP_JINGLE_DEFAULT]    = JINGLE_ENTRY(SOUND_STARTUP),
    [JPP_STARTUP_JINGLE_WINXP]      = JINGLE_ENTRY(JINGLE_WINXP),
    [JPP_STARTUP_JINGLE_WIN31]      = JINGLE_ENTRY(JINGLE_WIN31),
    [JPP_STARTUP_JINGLE_MAC]        = JINGLE_ENTRY(JINGLE_MAC),
    [JPP_STARTUP_JINGLE_RICKROLL]   = JINGLE_ENTRY(JINGLE_RICKROLL),
    [JPP_STARTUP_JINGLE_NOKIA_ON]   = JINGLE_ENTRY(JINGLE_NOKIA_ON),
    [JPP_STARTUP_JINGLE_NOKIA_TUNE] = JINGLE_ENTRY(JINGLE_NOKIA_TUNE),
    [JPP_STARTUP_JINGLE_SANDSTORM]  = JINGLE_ENTRY(JINGLE_SANDSTORM),
    [JPP_STARTUP_JINGLE_DOOM]       = JINGLE_ENTRY(JINGLE_DOOM),
    [JPP_STARTUP_JINGLE_CLUTTERFUNK]= JINGLE_ENTRY(JINGLE_CLUTTERFUNK),
    [JPP_STARTUP_JINGLE_OFF]        = {NULL, 0},
};

/* ---- Async player task -------------------------------------------------- */
/*
 * Blocking playback (jpp_buzzer_tone / _play_sequence) freezes the calling
 * task for the whole sequence — fine for an app that wants to wait, but bad
 * for the UI (e.g. cycling startup jingles in Settings, where Rick Roll is
 * ~4 s).  The async API copies the sequence into an inbox buffer and wakes a
 * dedicated low-priority player task.  Submitting a new sequence (or calling
 * jpp_buzzer_stop) bumps a generation counter; the player checks it between
 * notes and abandons a superseded sequence, so a fresh preview cuts the
 * previous one off within one note.
 */

#ifndef JPP_BUZZER_SEQ_MAX
#define JPP_BUZZER_SEQ_MAX          64u
#endif
#ifndef JPP_BUZZER_TASK_STACK_BYTES
#define JPP_BUZZER_TASK_STACK_BYTES 3072u
#endif
#ifndef JPP_BUZZER_TASK_PRIO
#define JPP_BUZZER_TASK_PRIO        5u
#endif

static jpp_buzzer_note_t s_req_buf[JPP_BUZZER_SEQ_MAX];   /* inbox (writer: caller) */
static size_t            s_req_count;
static volatile uint32_t s_seq_gen;                       /* bumped on submit + stop */
static jpp_buzzer_note_t s_play_buf[JPP_BUZZER_SEQ_MAX];  /* player's working copy */

static SemaphoreHandle_t s_seq_mutex;
static StaticSemaphore_t s_seq_mutex_buf;
static TaskHandle_t      s_player_task;
static StackType_t       s_player_stack[JPP_BUZZER_TASK_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t      s_player_tcb;

/* Silence the output without touching async state (used internally by the
   per-note playback path; must NOT cancel an in-flight async sequence). */
static void buzzer_silence(void)
{
    if (!s_initialized) { return; }
    ledc_set_duty(JPP_HW_BUZZER_LEDC_MODE, JPP_HW_BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(JPP_HW_BUZZER_LEDC_MODE, JPP_HW_BUZZER_LEDC_CHANNEL);
}

static void buzzer_player_task(void *arg)
{
    (void)arg;
    uint32_t last_played = 0u;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        size_t   count;
        uint32_t my_gen;
        xSemaphoreTake(s_seq_mutex, portMAX_DELAY);
        count  = s_req_count;
        my_gen = s_seq_gen;
        if (count > JPP_BUZZER_SEQ_MAX) { count = JPP_BUZZER_SEQ_MAX; }
        memcpy(s_play_buf, s_req_buf, count * sizeof(s_play_buf[0]));
        xSemaphoreGive(s_seq_mutex);

        /* Dedupe: if a stale notification fires for a generation we already
           played (e.g. two submits racing the wake), don't replay it. */
        if (my_gen == last_played) { continue; }
        last_played = my_gen;

        for (size_t i = 0u; i < count; i++) {
            if (s_seq_gen != my_gen) { break; }  /* superseded or stopped */
            jpp_buzzer_tone(s_play_buf[i].freq_hz, s_play_buf[i].duration_ms);
        }
        buzzer_silence();
    }
}

/* Copy a sequence into the inbox and wake the player.  Falls back to blocking
   playback if the player task could not be created. */
static jpp_buzzer_status_t buzzer_submit_async(const jpp_buzzer_note_t *notes,
                                               size_t count)
{
    if (!s_initialized) { return JPP_BUZZER_STATUS_NOT_INITIALIZED; }
    if (notes == NULL || count == 0u) {
        jpp_buzzer_stop();  /* nothing to play: cancel anything in flight */
        return JPP_BUZZER_STATUS_OK;
    }
    if (s_player_task == NULL || s_seq_mutex == NULL) {
        return jpp_buzzer_play_sequence(notes, count);
    }
    if (count > JPP_BUZZER_SEQ_MAX) { count = JPP_BUZZER_SEQ_MAX; }
    xSemaphoreTake(s_seq_mutex, portMAX_DELAY);
    memcpy(s_req_buf, notes, count * sizeof(s_req_buf[0]));
    s_req_count = count;
    s_seq_gen++;            /* abort whatever the player is doing now */
    xSemaphoreGive(s_seq_mutex);
    xTaskNotifyGive(s_player_task);
    return JPP_BUZZER_STATUS_OK;
}

/* ---- Init --------------------------------------------------------------- */

jpp_buzzer_status_t jpp_buzzer_init(void)
{
    /* Boost drive strength on the LP GPIO3 pad. */
    gpio_set_drive_capability((gpio_num_t)JPP_HW_BUZZER_GPIO, GPIO_DRIVE_CAP_3);

    ledc_timer_config_t tcfg = {
        .speed_mode      = JPP_HW_BUZZER_LEDC_MODE,
        .duty_resolution = JPP_HW_BUZZER_RESOLUTION,
        .timer_num       = JPP_HW_BUZZER_LEDC_TIMER,
        .freq_hz         = 1000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&tcfg) != ESP_OK) {
        return JPP_BUZZER_STATUS_INVALID_ARGUMENT;
    }

    ledc_channel_config_t ccfg = {
        .gpio_num   = JPP_HW_BUZZER_GPIO,
        .speed_mode = JPP_HW_BUZZER_LEDC_MODE,
        .channel    = JPP_HW_BUZZER_LEDC_CHANNEL,
        .timer_sel  = JPP_HW_BUZZER_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&ccfg) != ESP_OK) {
        return JPP_BUZZER_STATUS_INVALID_ARGUMENT;
    }

    s_initialized = true;

    /* Spin up the async player task (static allocation to avoid heap use).
       If creation fails, async calls transparently fall back to blocking. */
    s_seq_mutex = xSemaphoreCreateMutexStatic(&s_seq_mutex_buf);
    s_player_task = xTaskCreateStatic(buzzer_player_task, "buzzer",
                                      JPP_BUZZER_TASK_STACK_BYTES, NULL,
                                      JPP_BUZZER_TASK_PRIO,
                                      s_player_stack, &s_player_tcb);

    return JPP_BUZZER_STATUS_OK;
}

/* ---- Playback ----------------------------------------------------------- */

jpp_buzzer_status_t jpp_buzzer_set_volume(uint8_t percent)
{
    s_volume_pct = (percent > 100u) ? 100u : percent;
    /* Mute: drive capability is irrelevant; tone calls will produce silence. */
    if (s_volume_pct == 0u) { return JPP_BUZZER_STATUS_OK; }
    /* Map volume to GPIO drive strength.  Passive piezos are capacitive loads:
       lower drive current limits the voltage swing the element actually sees,
       producing real amplitude reduction without the harmonic distortion caused
       by changing duty cycle away from 50 %. */
    gpio_drive_cap_t cap;
    if      (s_volume_pct >= 100u) cap = GPIO_DRIVE_CAP_3; /* ~40 mA — max */
    else if (s_volume_pct >= 75u)  cap = GPIO_DRIVE_CAP_2; /* ~20 mA */
    else if (s_volume_pct >= 50u)  cap = GPIO_DRIVE_CAP_1; /* ~10 mA */
    else                           cap = GPIO_DRIVE_CAP_0; /* ~5 mA  — min */
    gpio_set_drive_capability((gpio_num_t)JPP_HW_BUZZER_GPIO, cap);
    return JPP_BUZZER_STATUS_OK;
}

uint8_t jpp_buzzer_get_volume(void)
{
    return s_volume_pct;
}

jpp_buzzer_status_t jpp_buzzer_stop(void)
{
    if (!s_initialized) { return JPP_BUZZER_STATUS_NOT_INITIALIZED; }
    /* Cancel any async sequence currently playing or queued. */
    if (s_seq_mutex != NULL) {
        xSemaphoreTake(s_seq_mutex, portMAX_DELAY);
        s_seq_gen++;
        s_req_count = 0u;
        xSemaphoreGive(s_seq_mutex);
    }
    buzzer_silence();
    return JPP_BUZZER_STATUS_OK;
}

jpp_buzzer_status_t jpp_buzzer_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!s_initialized) { return JPP_BUZZER_STATUS_NOT_INITIALIZED; }
    if (freq_hz == 0 || s_volume_pct == 0u) {
        buzzer_silence();
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        return JPP_BUZZER_STATUS_OK;
    }
    ledc_set_freq(JPP_HW_BUZZER_LEDC_MODE, JPP_HW_BUZZER_LEDC_TIMER, freq_hz);
    ledc_set_duty(JPP_HW_BUZZER_LEDC_MODE, JPP_HW_BUZZER_LEDC_CHANNEL,
                  JPP_HW_BUZZER_DUTY);
    ledc_update_duty(JPP_HW_BUZZER_LEDC_MODE, JPP_HW_BUZZER_LEDC_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    buzzer_silence();
    return JPP_BUZZER_STATUS_OK;
}

jpp_buzzer_status_t jpp_buzzer_play_sequence(const jpp_buzzer_note_t *notes,
                                              size_t count)
{
    if (!s_initialized) { return JPP_BUZZER_STATUS_NOT_INITIALIZED; }
    if (notes == NULL && count > 0u) { return JPP_BUZZER_STATUS_INVALID_ARGUMENT; }
    /* Preempt any async sequence so the player task and this blocking caller do
       not drive the LEDC channel at the same time.  The player loops on
       jpp_buzzer_tone (not this function), so bumping the generation here only
       cancels the async sequence, never our own playback. */
    if (s_seq_mutex != NULL) {
        xSemaphoreTake(s_seq_mutex, portMAX_DELAY);
        s_seq_gen++;
        s_req_count = 0u;
        xSemaphoreGive(s_seq_mutex);
    }
    for (size_t i = 0u; i < count; i++) {
        jpp_buzzer_tone(notes[i].freq_hz, notes[i].duration_ms);
    }
    return JPP_BUZZER_STATUS_OK;
}

jpp_buzzer_status_t jpp_buzzer_play(jpp_buzzer_sound_t sound)
{
    if (!s_initialized) { return JPP_BUZZER_STATUS_NOT_INITIALIZED; }
    if ((size_t)sound >= JPP_BUZZER_SOUND_COUNT) {
        return JPP_BUZZER_STATUS_INVALID_ARGUMENT;
    }
    return jpp_buzzer_play_sequence(SOUNDS[sound].notes, SOUNDS[sound].count);
}

/* ---- Async (non-blocking) playback -------------------------------------- */

jpp_buzzer_status_t jpp_buzzer_play_sequence_async(const jpp_buzzer_note_t *notes,
                                                   size_t count)
{
    return buzzer_submit_async(notes, count);
}

jpp_buzzer_status_t jpp_buzzer_play_async(jpp_buzzer_sound_t sound)
{
    if ((size_t)sound >= JPP_BUZZER_SOUND_COUNT) {
        return JPP_BUZZER_STATUS_INVALID_ARGUMENT;
    }
    return buzzer_submit_async(SOUNDS[sound].notes, SOUNDS[sound].count);
}

/* ---- Name helpers ------------------------------------------------------- */

const char *jpp_buzzer_status_name(jpp_buzzer_status_t status)
{
    switch (status) {
    case JPP_BUZZER_STATUS_OK:               return "OK";
    case JPP_BUZZER_STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case JPP_BUZZER_STATUS_NOT_INITIALIZED:  return "NOT_INITIALIZED";
    }
    return "UNKNOWN";
}

const char *jpp_buzzer_sound_name(jpp_buzzer_sound_t sound)
{
    switch (sound) {
    case JPP_BUZZER_SOUND_SUCCESS: return "SUCCESS";
    case JPP_BUZZER_SOUND_FAILURE: return "FAILURE";
    case JPP_BUZZER_SOUND_NOTIFY:  return "NOTIFY";
    case JPP_BUZZER_SOUND_STARTUP: return "STARTUP";
    case JPP_BUZZER_SOUND_CLICK:   return "CLICK";
    default: break;
    }
    return "UNKNOWN";
}

jpp_buzzer_status_t jpp_buzzer_play_startup_jingle(jpp_startup_jingle_t jingle)
{
    if ((size_t)jingle >= JPP_STARTUP_JINGLE_COUNT) {
        return JPP_BUZZER_STATUS_INVALID_ARGUMENT;
    }
    if (JINGLES[jingle].notes == NULL || JINGLES[jingle].count == 0u) {
        return JPP_BUZZER_STATUS_OK; /* OFF or empty */
    }
    return jpp_buzzer_play_sequence(JINGLES[jingle].notes, JINGLES[jingle].count);
}

jpp_buzzer_status_t jpp_buzzer_play_startup_jingle_async(jpp_startup_jingle_t jingle)
{
    if ((size_t)jingle >= JPP_STARTUP_JINGLE_COUNT) {
        return JPP_BUZZER_STATUS_INVALID_ARGUMENT;
    }
    if (JINGLES[jingle].notes == NULL || JINGLES[jingle].count == 0u) {
        jpp_buzzer_stop();           /* OFF: cancel any preview in flight */
        return JPP_BUZZER_STATUS_OK;
    }
    return buzzer_submit_async(JINGLES[jingle].notes, JINGLES[jingle].count);
}

const char *jpp_startup_jingle_name(jpp_startup_jingle_t jingle)
{
    switch (jingle) {
    case JPP_STARTUP_JINGLE_DEFAULT:     return "Default";
    case JPP_STARTUP_JINGLE_WINXP:       return "WinXP Startup";
    case JPP_STARTUP_JINGLE_WIN31:       return "Win3.1 Startup";
    case JPP_STARTUP_JINGLE_MAC:         return "Mac128k";
    case JPP_STARTUP_JINGLE_RICKROLL:    return "Rick Roll";
    case JPP_STARTUP_JINGLE_NOKIA_ON:    return "Nokia Power On";
    case JPP_STARTUP_JINGLE_NOKIA_TUNE:  return "Nokia Tune";
    case JPP_STARTUP_JINGLE_SANDSTORM:   return "Sandstorm";
    case JPP_STARTUP_JINGLE_DOOM:        return "DOOM";
    case JPP_STARTUP_JINGLE_CLUTTERFUNK: return "Clutterfunk";
    case JPP_STARTUP_JINGLE_OFF:         return "OFF";
    default: break;
    }
    return "Unknown";
}
