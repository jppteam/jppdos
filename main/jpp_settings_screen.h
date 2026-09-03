#pragma once

/*
 * Settings application — privileged built-in screen.
 * Renders directly to the SSD1306 and handles d-pad input.
 * Lives in main/ so it can use ssd1306.h, jpp_rtc_core.h, etc. without
 * creating a circular component dependency.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpp_ui_core.h"
#include "jpp_rtc_core.h"
#include "jpp_kbd_core.h"
#include "jpp_buzzer_core.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Version string ----------------------------------------------------- */
#define JPPDOS_VERSION "1.3"

/* ---- Settings sections -------------------------------------------------- */
typedef enum {
    JPP_SETTINGS_SECTION_SHUTDOWN_REBOOT = 0,
    /* Back action, user's name, system apps location */
    JPP_SETTINGS_SECTION_PERSONALISATION,
    JPP_SETTINGS_SECTION_WIFI,
    JPP_SETTINGS_SECTION_TIME,
    JPP_SETTINGS_SECTION_SLEEP_TIMERS,
    JPP_SETTINGS_SECTION_SOUND,       /* buzzer volume, startup jingle, test */
    JPP_SETTINGS_SECTION_SD_CARD,
    JPP_SETTINGS_SECTION_BACKUP,
    JPP_SETTINGS_SECTION_FACTORY_RESET,
    JPP_SETTINGS_SECTION_DEVICE_INFO, /* hidden when no LRV data present */
    JPP_SETTINGS_SECTION_DUMMY_MODE,  /* single-app lock; disable by holding OK on boot */
    JPP_SETTINGS_SECTION_ABOUT,
    JPP_SETTINGS_SECTION_COUNT,
} jpp_settings_section_t;

typedef enum {
    JPP_LRV_SS_MAIN = 0,       /* unit #, pubkey, "OK to verify validity"         */
    JPP_LRV_SS_VERIFY_RESULT,  /* "Printed certificate to serial." screen         */
    JPP_LRV_SS_VERIFY_ERROR,   /* error screen (e.g. WebDAV running)              */
} jpp_lrv_subscreen_t;

typedef enum {
    JPP_WIFI_SS_MAIN = 0,
    JPP_WIFI_SS_SCANNING,
    JPP_WIFI_SS_NETWORK_LIST,
    JPP_WIFI_SS_CONNECTING,
    JPP_WIFI_SS_KNOWN_NETWORKS,
} jpp_wifi_subscreen_t;

typedef enum {
    JPP_TIME_SS_MAIN = 0,
    JPP_TIME_SS_LIST,
    JPP_TIME_SS_TIMEZONE,
} jpp_time_subscreen_t;

typedef enum {
    JPP_BACKUP_SS_MAIN = 0,   /* two-item list: Backup / Restore */
    JPP_BACKUP_SS_RESULT,     /* show result message; any key dismisses */
} jpp_backup_subscreen_t;

#define JPP_SETTINGS_WIFI_SCAN_MAX   8u
#define JPP_SETTINGS_SSID_MAX        33u
#define JPP_SETTINGS_PASSWORD_MAX    65u
#define JPP_SETTINGS_NTP_HOST_MAX    65u
#define JPP_SETTINGS_USERNAME_MAX    64u

typedef struct {
    char  ssid[JPP_SETTINGS_SSID_MAX];
    int8_t rssi;
    bool  has_password;
} jpp_settings_wifi_ap_t;

typedef struct {
    jpp_settings_section_t selected_section;
    bool section_open;

    /* Wi-Fi */
    jpp_wifi_subscreen_t wifi_ss;
    bool   wifi_scan_pending;    /* true = render flushed "Scanning...", trigger scan next */
    bool   wifi_connect_pending; /* true = render flushed "Connecting...", trigger connect next */
    jpp_settings_wifi_ap_t wifi_scan_results[JPP_SETTINGS_WIFI_SCAN_MAX];
    size_t wifi_scan_count;
    size_t wifi_list_cursor;
    size_t wifi_list_scroll;     /* index of first visible list item */
    char   wifi_connecting_ssid[JPP_SETTINGS_SSID_MAX];
    char   wifi_pending_password[JPP_SETTINGS_PASSWORD_MAX]; /* stored for deferred connect */
    char   wifi_status_msg[48];
    char   wifi_saved_ssid[JPP_SETTINGS_SSID_MAX]; /* SSID persisted in settings.json */
    bool   wifi_is_connecting; /* true while auto-reconnect loop is active (not yet connected) */

    /* Time */
    jpp_time_subscreen_t time_ss;
    size_t time_list_cursor;   /* 0=NTP sync, 1=NTP server, 2=Timezone */
    bool   ntp_enabled_staging;
    char   ntp_host_staging[JPP_SETTINGS_NTP_HOST_MAX];
    int    timezone_offset_h;  /* -12 … +14, UTC offset in whole hours */

    /* Factory reset */
    size_t factory_reset_cursor;

    /* Sleep timers section */
    size_t sleep_timers_cursor;

    /* Shutdown/Reboot section */
    size_t shutdown_reboot_cursor;

    /* Backup settings section */
    jpp_backup_subscreen_t backup_ss;
    size_t backup_cursor;           /* 0 = Backup to SD card, 1 = Restore from file */
    char   backup_result_msg[64];   /* status message shown in RESULT subscreen */

    /* Sound section */
    size_t  sound_cursor;           /* 0 = Volume, 1 = Jingle, 2 = Test */
    uint8_t sound_volume_pct;       /* active volume: 0 / 25 / 50 / 75 / 100 */
    uint8_t sound_jingle;           /* selected startup jingle (jpp_startup_jingle_t) */

    /* Personalisation section */
    size_t  personalisation_cursor; /* 0 = Back action, 1 = User's name, 2 = System apps */
    uint8_t back_gesture_mode;      /* jpp_keypad_back_gesture_t: 0=Hold, 1=Double-click */
    bool    system_apps_bottom;     /* launcher: system apps at the bottom of the list */

    /* Device Info / LRV section */
    bool               lrv_has_data;
    jpp_lrv_subscreen_t lrv_ss;
    char               lrv_verify_error[40];
    uint16_t           lrv_serial;
    char               lrv_pubkey_str[16];   /* "ABCDEF-GHIJKL" */
    bool               lrv_server_running;
    char               lrv_server_addr[28];  /* "x.x.x.x:3000\0" */

    /* User's name (Personalisation section) */
    char               username_current[JPP_SETTINGS_USERNAME_MAX];

    /* Dummy Mode section */
    bool   dummy_enabled;
    char   dummy_app_id[JPP_UI_TEXT_LIMIT];
    char   dummy_app_name[JPP_UI_TEXT_LIMIT];
    size_t dummy_cursor;
    size_t dummy_scroll;
} jpp_settings_state_t;

typedef struct {
    jpp_ui_shell_t  *shell;
    jpp_rtc_state_t *rtc;
    sdmmc_card_t   **sd_card_ptr;   /* pointer to mounted sdmmc_card_t* or NULL */

    /* Callbacks */
    void (*do_wifi_scan)(jpp_settings_state_t *state);
    /* do_wifi_connect: blocking connect; updates state->wifi_status_msg on return. */
    void (*do_wifi_connect)(jpp_settings_state_t *state,
                             const char *ssid, const char *password);
    void (*do_wifi_disconnect)(jpp_settings_state_t *state);
    /* do_wifi_check_status: populate wifi_status_msg from actual connection state. */
    void (*do_wifi_check_status)(jpp_settings_state_t *state);
    void (*do_factory_reset)(void);
    void (*do_ntp_save)(bool enabled, const char *host, int tz_offset_h);
    void (*do_dim_time_change)(int32_t seconds);
    void (*do_poweroff_time_change)(int32_t seconds);
    void (*do_reboot)(void);
    void (*do_shutdown)(void);
    /* Input UI callback: prompt user for a string (uses on-screen keyboard).
       Returns true and fills `out` if user confirmed; returns false if cancelled. */
    bool (*do_text_input)(const char *title, const char *prefill,
                          jpp_kbd_input_type_t type,
                          char *out, size_t out_len);
    /* Called when the user changes buzzer volume in the Sound section.
       Applies the new level immediately and persists it to NVS. */
    void (*do_volume_change)(uint8_t percent);
    /* Called when the user changes the startup jingle.  Persists to NVS.
       The new jingle is played immediately as a preview. */
    void (*do_jingle_change)(uint8_t jingle);
    /* Called when the user changes the Back button gesture (Hold/Double-click
       in the Personalisation section). Applies live and persists to NVS. */
    void (*do_back_gesture_change)(uint8_t mode);
    /* Called when the user moves the system apps to the top or bottom of the
       launcher list. Re-orders the shell catalogue live and persists to NVS. */
    void (*do_system_apps_pos_change)(bool bottom);

    /* Backup all settings (settings.json + NVS) to the SD card.
       Writes a human-readable result into state->backup_result_msg on return. */
    void (*do_settings_backup)(jpp_settings_state_t *state);
    /* Invoke the file picker and restore settings from the chosen .json file.
       Shows the picker (blocking), restores on success (may restart the device),
       or writes an error into state->backup_result_msg and returns normally. */
    void (*do_settings_restore)(jpp_settings_state_t *state);

    /* LRV: log the certificate and start the HTTP verification server.
       Populates state->lrv_server_running and state->lrv_server_addr.
       On error populates state->lrv_verify_error. */
    void (*do_lrv_verify)(jpp_settings_state_t *state);
    /* LRV: stop the HTTP verification server if running. */
    void (*do_lrv_server_stop)(void);

    /* User's name (Personalisation): persist the new username.  Also populates
       state->username_current with the saved value. */
    void (*do_username_save)(jpp_settings_state_t *state, const char *username);

    /* Dummy Mode: enable or disable single-app lock.  When enabled, app_id
       is the SD app to lock to; when disabled, app_id is ignored.
       The settings handler restarts the device after enabling (via do_reboot). */
    void (*do_dummy_mode_save)(bool enabled, const char *app_id);
} jpp_settings_deps_t;

void jpp_settings_screen_init(jpp_settings_state_t *state,
                               const jpp_settings_deps_t *deps);

void jpp_settings_screen_render(jpp_settings_state_t *state,
                                 const jpp_settings_deps_t *deps);

/* Returns true if settings screen should be popped (user pressed BACK at top). */
bool jpp_settings_screen_handle_action(jpp_settings_state_t *state,
                                        const jpp_settings_deps_t *deps,
                                        jpp_ui_action_t action);

#ifdef __cplusplus
}
#endif
