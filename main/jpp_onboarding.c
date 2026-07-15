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

/* Defined in app_main.c; same extern-declare pattern jpp_app_dispatch.c uses. */
extern QueueHandle_t s_action_queue;

#define ONBOARD_NVS_NS  "jpp_onboard"
#define ONBOARD_NVS_KEY "done"
#define USER_NVS_NS     "jpp_user"
#define USER_NVS_KEY    "username"

static jpp_ui_action_t wait_key(jpp_ui_shell_t *shell)
{
    jpp_ui_action_t action;
    xQueueReceive(s_action_queue, &action, portMAX_DELAY);
    if (shell != NULL) {
        jpp_ui_shell_note_activity(shell);
    }
    return action;
}

/* Screen 1: "Welcome to J++Device! / This is unit NN/20 / Press OK to set
   username" — the unit line is omitted unless LRV data is present and
   unlocked. Blocks on OK only; first boot is not skippable. */
static void screen_welcome(jpp_ui_shell_t *shell)
{
    uint16_t serial = 0u;
    uint16_t run_size = 0u;
    bool has_unit = jpp_lrv_has_data() && jpp_lrv_is_unlocked();
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
        ssd1306_draw_string(row++, 0u, "Press OK to set", false);
        ssd1306_draw_string(row++, 0u, "username", false);
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
static bool screen_wifi_prompt(jpp_ui_shell_t *shell, const char *username)
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
        ssd1306_flush();

        switch (wait_key(shell)) {
        case JPP_UI_ACTION_UP:   cursor = 0; break;
        case JPP_UI_ACTION_DOWN: cursor = 1; break;
        case JPP_UI_ACTION_OK:   return cursor == 0;
        default: break;
        }
    }
}

void jpp_onboarding_run(jpp_ui_shell_t *shell, jpp_settings_state_t *settings_state)
{
    if (jpp_nvs_get_u8(ONBOARD_NVS_NS, ONBOARD_NVS_KEY, 0u)) {
        return;
    }

    screen_welcome(shell);
    screen_username(shell, settings_state);
    bool connect_wifi = screen_wifi_prompt(shell, settings_state->username_current);
    if (connect_wifi && shell != NULL) {
        jpp_ui_stack_push(&shell->stack, "settings");
    }

    jpp_nvs_set_u8(ONBOARD_NVS_NS, ONBOARD_NVS_KEY, 1u);
}
