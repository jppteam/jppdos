#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpp_keypad_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_UI_FRAME_LINES 8u
#define JPP_UI_FRAME_CHARS 21u
#define JPP_UI_STACK_LIMIT 8u
#define JPP_UI_APP_LIMIT 8u
#define JPP_UI_TEXT_LIMIT 32u
#define JPP_UI_DIALOG_BODY_LINES 4u
#define JPP_UI_CRASH_LOG_CHARS 256u
#define JPP_UI_STATUS_TIME_LEN 6u    /* "HH:MM\0" */
#define JPP_UI_WEBDAV_PASS_MAX 24u   /* max WebDAV password length */

typedef enum {
    JPP_UI_STATUS_OK = 0,
    JPP_UI_STATUS_INVALID_ARGUMENT,
    JPP_UI_STATUS_STACK_OVERFLOW,
    JPP_UI_STATUS_CATALOG_FULL,
} jpp_ui_status_t;

typedef enum {
    JPP_UI_ACTION_NONE = 0,
    JPP_UI_ACTION_UP,
    JPP_UI_ACTION_DOWN,
    JPP_UI_ACTION_LEFT,
    JPP_UI_ACTION_RIGHT,
    JPP_UI_ACTION_OK,
    JPP_UI_ACTION_BACK,
} jpp_ui_action_t;

typedef struct {
    char lines[JPP_UI_FRAME_LINES][JPP_UI_FRAME_CHARS + 1u];
} jpp_ui_frame_t;

typedef struct {
    jpp_ui_frame_t last_frame;
    bool has_last_frame;
} jpp_ui_display_t;

typedef struct {
    const char *screens[JPP_UI_STACK_LIMIT];
    size_t depth;
} jpp_ui_stack_t;

typedef enum {
    JPP_UI_APP_SOURCE_BUILTIN = 0,
    JPP_UI_APP_SOURCE_SD,
} jpp_ui_app_source_t;

typedef struct {
    char     app_id[JPP_UI_TEXT_LIMIT];
    char     name[JPP_UI_TEXT_LIMIT];
    jpp_ui_app_source_t source;
} jpp_ui_app_entry_t;

typedef struct {
    char title[JPP_UI_TEXT_LIMIT];
    char body[JPP_UI_DIALOG_BODY_LINES][JPP_UI_TEXT_LIMIT];
    size_t body_count;
} jpp_ui_dialog_t;

/* Screen dim / sleep state tracked by the shell. */
typedef enum {
    JPP_UI_POWER_ACTIVE = 0,
    JPP_UI_POWER_DIM,    /* screen dimmed; show clock on launcher */
    JPP_UI_POWER_OFF,    /* screen off; deep sleep; full reset on keypress */
} jpp_ui_power_state_t;

typedef struct {
    jpp_ui_display_t display;
    jpp_ui_stack_t stack;
    jpp_ui_app_entry_t apps[JPP_UI_APP_LIMIT];
    size_t app_count;
    /* Launcher order: false = system (builtin) apps first, true = last.
       apps[] is always kept in display order, so nothing else has to know. */
    bool   system_apps_bottom;
    /* Launcher: draw the system/SD divider row. Defaults to true; with it off
       the divider takes no display row and the list shows six apps again. */
    bool   show_divider;
    size_t selected_app;
    size_t selected_setting;
    char boot_mode[JPP_UI_TEXT_LIMIT];
    char last_opened_app_id[JPP_UI_TEXT_LIMIT];
    char crash_log_path[JPP_UI_TEXT_LIMIT];
    char crash_log[JPP_UI_CRASH_LOG_CHARS];
    jpp_ui_dialog_t dialog;
    /* Status bar state */
    char status_time[JPP_UI_STATUS_TIME_LEN];   /* "HH:MM" or "" */
    int  status_battery_pct;                     /* 0–100, -1 = unknown */
    bool status_wifi_connected;                  /* true when Wi-Fi has an IP */
    /* File server state */
    bool     fileserver_running;
    char     fileserver_ip[16];
    uint16_t fileserver_port;
    char     fileserver_password[JPP_UI_WEBDAV_PASS_MAX + 1u];
    /* WebDAV screen navigation and password config */
    size_t webdav_menu_sel;                           /* 0=Password settings, 1=Stop server */
    size_t webdav_passconfig_sel;                     /* 0=Random password, 1=Static password */
    bool   webdav_pass_is_static;                     /* true = use static pass on next start */
    char   webdav_static_pass[JPP_UI_WEBDAV_PASS_MAX + 1u];
    bool   webdav_needs_pass_input;                   /* signal to main loop: invoke keyboard */
    bool   webdav_pass_config_changed;                /* signal to main loop: save config to NVS */
    /* SD ejection fatal flag */
    bool sd_ejected;
    /* Power management */
    jpp_ui_power_state_t power_state;
    int32_t dim_time_s;       /* seconds of inactivity before dim (0=never) */
    int32_t poweroff_time_s;  /* seconds of inactivity before deep sleep (0=never) */
    int64_t last_activity_us; /* esp_timer_get_time() of last keypress */
    /* Launcher "can't go back" hint: render-tick countdown, 0 = hidden */
    uint32_t back_at_root_flash;
} jpp_ui_shell_t;

void jpp_ui_frame_clear(jpp_ui_frame_t *frame);
jpp_ui_status_t jpp_ui_frame_from_lines(
    jpp_ui_frame_t *frame,
    const char *const *lines,
    size_t line_count
);
void jpp_ui_display_init(jpp_ui_display_t *display);
jpp_ui_status_t jpp_ui_display_render_lines(
    jpp_ui_display_t *display,
    const char *const *lines,
    size_t line_count,
    jpp_ui_frame_t *rendered_frame,
    bool *changed
);
/* Maps a keypad event to a UI action. OK hold/double-click gestures are
   NOT handled here — they are policy (see the note in the implementation) and
   are resolved by keypad_task() before this is called. */
jpp_ui_action_t jpp_ui_normalize_action(const jpp_keypad_event_t *event);
const char *jpp_ui_action_name(jpp_ui_action_t action);
const char *jpp_ui_status_name(jpp_ui_status_t status);
jpp_ui_status_t jpp_ui_stack_init(jpp_ui_stack_t *stack, const char *root_screen);
jpp_ui_status_t jpp_ui_stack_push(jpp_ui_stack_t *stack, const char *screen);
const char *jpp_ui_stack_pop(jpp_ui_stack_t *stack);
const char *jpp_ui_stack_top(const jpp_ui_stack_t *stack);
jpp_ui_status_t jpp_ui_stack_reset_to_root(jpp_ui_stack_t *stack);
void jpp_ui_shell_init(jpp_ui_shell_t *shell, const char *boot_mode);
/* Remove all non-builtin apps from the shell's app list.
   The cursor is clamped to the remaining count. */
void jpp_ui_shell_clear_sd_apps(jpp_ui_shell_t *shell);
jpp_ui_status_t jpp_ui_shell_add_app(
    jpp_ui_shell_t *shell,
    const char *app_id,
    const char *name,
    jpp_ui_app_source_t source
);
/* Move the system (builtin) app group to the bottom of the launcher list, or
   back to the top.  Re-orders apps[] in place, keeping the cursor on whichever
   app it was already on. */
void jpp_ui_shell_set_system_apps_bottom(jpp_ui_shell_t *shell, bool bottom);

/* Frame row (0–7) whose bottom pixel row carries the divider between the
   system apps and the SD apps on the launcher, or -1 when the divider is
   switched off, there is nothing to divide (one of the two groups is empty),
   or the boundary is scrolled out of view.  The display layer draws the rule
   — see jpp_draw_divider_rule(). */
int jpp_ui_shell_launcher_divider_row(const jpp_ui_shell_t *shell);

void jpp_ui_shell_set_status(
    jpp_ui_shell_t *shell,
    const char *time_str,
    int battery_pct
);
jpp_ui_status_t jpp_ui_shell_render(
    jpp_ui_shell_t *shell,
    jpp_ui_frame_t *frame,
    bool *changed
);
jpp_ui_status_t jpp_ui_shell_handle_action(jpp_ui_shell_t *shell, jpp_ui_action_t action);

/* Notify of an activity event (resets inactivity timer, restores dim/off state). */
void jpp_ui_shell_note_activity(jpp_ui_shell_t *shell);

/* Tick power-management state (call once per UI refresh cycle).
   Returns true if the power state changed (caller should act accordingly). */
bool jpp_ui_shell_tick_power(jpp_ui_shell_t *shell);

jpp_ui_status_t jpp_ui_shell_show_dialog(
    jpp_ui_shell_t *shell,
    const char *title,
    const char *const *body_lines,
    size_t body_count
);
jpp_ui_status_t jpp_ui_shell_record_crash(jpp_ui_shell_t *shell, const char *title,
                                           const char *screen, const char *error_name);
void jpp_ui_shell_set_fileserver_state(
    jpp_ui_shell_t *shell,
    bool            running,
    const char     *ip,
    uint16_t        port,
    const char     *password
);
const char *jpp_ui_shell_screen(const jpp_ui_shell_t *shell);
const char *jpp_ui_shell_selected_app_id(const jpp_ui_shell_t *shell);

/* ---- Generic list-view helpers -------------------------------------------- */

/* Clamp a list scroll offset so the cursor row stays visible and the window
   never extends past the last row. Returns the updated scroll offset. */
size_t jpp_ui_scroll_clamp(size_t cursor, size_t total, size_t visible, size_t scroll);

/* Horizontal marquee for text wider than its window: hold for pause_ticks at
   each end, scroll one character per tick in between. Returns the index of
   the first visible character for the given animation tick. */
size_t jpp_ui_marquee_offset(uint32_t tick, size_t text_len, size_t window, size_t pause_ticks);

/* The Deny/Allow selector row shared by every consent surface (capability
   prompts, file-access prompts, the serial-manager session dialog): ">" marks
   the highlighted side. */
const char *jpp_ui_consent_selector_row(bool allow);

#ifdef __cplusplus
}
#endif
