#include "jpp_onboarding.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "ssd1306.h"
#include "jpp_draw_util.h"
#include "jpp_keyboard.h"
#include "jpp_nvs_util.h"
#include "jpp_lrv.h"
#include "jpp_rtc_core.h"

/* Defined in app_main.c; same extern-declare pattern jpp_app_dispatch.c uses. */
extern QueueHandle_t s_action_queue;

#define ONBOARD_NVS_NS  "jpp_onboard"
#define ONBOARD_NVS_KEY "done"
#define USER_NVS_NS     "jpp_user"
#define USER_NVS_KEY    "username"

/* Status line: last display row, redrawn at least once a second so the clock
   ticks even while the user just sits on a screen. */
#define ONBOARD_STATUS_ROW    (JPP_UI_FRAME_LINES - 1u)
#define ONBOARD_REDRAW_MS     1000u

/* Blocks for at most ONBOARD_REDRAW_MS so the caller re-renders (and the
   status clock advances) while nothing is pressed; JPP_UI_ACTION_NONE means
   "nothing happened, draw again". */
static jpp_ui_action_t wait_key(jpp_ui_shell_t *shell)
{
    jpp_ui_action_t action;
    if (xQueueReceive(s_action_queue, &action,
                      pdMS_TO_TICKS(ONBOARD_REDRAW_MS)) != pdTRUE) {
        return JPP_UI_ACTION_NONE;
    }
    if (shell != NULL) {
        jpp_ui_shell_note_activity(shell);
    }
    return action;
}

/* Status line shared by the two title screens: current time on the left,
   battery percentage on the right.  Column layout mirrors the launcher status
   bar (jpp_ui_build_status_bar) except that the percentage is flush right —
   onboarding draws no Wi-Fi/battery icons to leave room for.  An unknown time
   renders "--:--" like every other clock in the firmware; an unknown battery
   percentage (-1) renders blank, as it does in the launcher. */
static void draw_status_line(const jpp_ui_shell_t *shell, const jpp_rtc_state_t *rtc)
{
    char line[JPP_UI_FRAME_CHARS + 1u];
    /* Sized to comfortably exceed the widest an int can print in either field
       below, so gcc's -Wformat-truncation can't flag a possible truncation;
       both are then memcpy'd into `line` at a fixed width anyway. */
    char field[24];
    jpp_rtc_datetime_t now;

    memset(line, ' ', JPP_UI_FRAME_CHARS);
    line[JPP_UI_FRAME_CHARS] = '\0';

    if (rtc != NULL && jpp_rtc_get_current(rtc, &now) == JPP_RTC_STATUS_OK) {
        snprintf(field, sizeof(field), "%02d:%02d", now.hour, now.minute);
    } else {
        snprintf(field, sizeof(field), "--:--");
    }
    memcpy(line, field, 5u);

    if (shell != NULL && shell->status_battery_pct >= 0) {
        snprintf(field, sizeof(field), "%3d%%", shell->status_battery_pct);
        memcpy(line + JPP_UI_FRAME_CHARS - 4u, field, 4u);
    }

    ssd1306_draw_string(ONBOARD_STATUS_ROW, 0u, line, false);
}

/* Screen 1: "Welcome to J++Device! / This is unit NN/20 / Press OK to set
   username" — the unit line is omitted unless LRV data is present. Blocks on
   OK only; first boot is not skippable. */
static void screen_welcome(jpp_ui_shell_t *shell, const jpp_rtc_state_t *rtc)
{
    uint16_t serial = 0u;
    uint16_t run_size = 0u;
    bool has_unit = jpp_lrv_has_data();
    if (has_unit) {
        char pubkey_str[16];
        jpp_lrv_get_display_info(&serial, pubkey_str);
        has_unit = jpp_lrv_get_run_size(&run_size) == JPP_LRV_OK;
    }

    for (;;) {
        ssd1306_clear();
        jpp_draw_title("Welcome to J++Device!");
        uint8_t row = 2u;
        if (has_unit) {
            char line[32];
            snprintf(line, sizeof(line), "This is unit %02u/%u", serial, run_size);
            ssd1306_draw_string(row++, 0u, line, false);
        }
        row++;
        ssd1306_draw_string(row++, 0u, "Press OK to set", false);
        ssd1306_draw_string(row++, 0u, "username", false);
        draw_status_line(shell, rtc);
        ssd1306_flush();

        if (wait_key(shell) == JPP_UI_ACTION_OK) {
            return;
        }
    }
}

/* Screen 2: username text input. Optional — cancelling or confirming empty
   leaves the username unset and onboarding continues. */
static void screen_username(jpp_ui_shell_t *shell, jpp_settings_state_t *state)
{
    char buf[JPP_SETTINGS_USERNAME_MAX];
    buf[0] = '\0';
    bool confirmed = jpp_keyboard_input("Set username", NULL, JPP_KBD_TYPE_TEXT,
                                        shell, buf, sizeof(buf));
    if (confirmed && buf[0] != '\0') {
        jpp_nvs_set_str(USER_NVS_NS, USER_NVS_KEY, buf);
        strncpy(state->username_current, buf, sizeof(state->username_current) - 1u);
        state->username_current[sizeof(state->username_current) - 1u] = '\0';
    }
}

/* Screen 3: "Hello, {username}! / Connect to Wi-Fi now? / > Yes / No".
   Returns true if the user picked Yes. */
static bool screen_wifi_prompt(jpp_ui_shell_t *shell, const char *username,
                                const jpp_rtc_state_t *rtc)
{
    int cursor = 0; /* 0 = Yes, 1 = No */
    for (;;) {
        ssd1306_clear();
        jpp_draw_title("Welcome to J++Device!");
        /* Sized to comfortably exceed "Hello, " + JPP_SETTINGS_USERNAME_MAX +
           "!" so gcc's -Wformat-truncation can't flag a possible truncation;
           the display itself only shows the first ~21 chars of any row. */
        char greet[JPP_SETTINGS_USERNAME_MAX + 16u];
        if (username != NULL && username[0] != '\0') {
            snprintf(greet, sizeof(greet), "Hello, %s!", username);
        } else {
            snprintf(greet, sizeof(greet), "Welcome!");
        }
        ssd1306_draw_string(2u, 0u, greet, false);
        ssd1306_draw_string(3u, 0u, "Connect to Wi-Fi now?", false);
        ssd1306_draw_string(4u, 0u, cursor == 0 ? "> Yes" : "  Yes", false);
        ssd1306_draw_string(5u, 0u, cursor == 1 ? "> No"  : "  No",  false);
        draw_status_line(shell, rtc);
        ssd1306_flush();

        switch (wait_key(shell)) {
        case JPP_UI_ACTION_UP:   cursor = 0; break;
        case JPP_UI_ACTION_DOWN: cursor = 1; break;
        case JPP_UI_ACTION_OK:   return cursor == 0;
        default: break;
        }
    }
}

void jpp_onboarding_run(jpp_ui_shell_t *shell, jpp_settings_state_t *settings_state,
                        const jpp_rtc_state_t *rtc)
{
    if (jpp_nvs_get_u8(ONBOARD_NVS_NS, ONBOARD_NVS_KEY, 0u)) {
        return;
    }

    screen_welcome(shell, rtc);
    screen_username(shell, settings_state);
    bool connect_wifi = screen_wifi_prompt(shell, settings_state->username_current, rtc);
    if (connect_wifi && shell != NULL) {
        jpp_ui_stack_push(&shell->stack, "settings");
    }

    jpp_nvs_set_u8(ONBOARD_NVS_NS, ONBOARD_NVS_KEY, 1u);
}
