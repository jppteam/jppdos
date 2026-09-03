#include "jpp_settings_screen.h"
#include "jpp_buzzer_core.h"
#include "jpp_draw_util.h"
#include "jpp_ui_core.h"
#include "ssd1306.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_vfs_fat.h"
#include "ff.h"

/* ---- Drawing helpers ---------------------------------------------------- */

static void cls(void) { ssd1306_clear(); }

static void draw_centred(uint8_t page, const char *str)
{
    size_t len = strlen(str);
    uint8_t col = (len * SSD1306_CHAR_W < SSD1306_WIDTH)
                  ? (uint8_t)((SSD1306_WIDTH - len * SSD1306_CHAR_W) / 2u)
                  : 0u;
    ssd1306_draw_string(page, col, str, false);
}

static void draw_centred_2x(uint8_t page, const char *str)
{
    size_t len = strlen(str);
    uint8_t col = (len * SSD1306_CHAR_W_2X < SSD1306_WIDTH)
                  ? (uint8_t)((SSD1306_WIDTH - len * SSD1306_CHAR_W_2X) / 2u)
                  : 0u;
    ssd1306_draw_string_2x(page, col, str, false);
}

static void draw_right(uint8_t page, const char *str)
{
    size_t len = strlen(str);
    uint8_t col = (len * SSD1306_CHAR_W < SSD1306_WIDTH)
                  ? (uint8_t)(SSD1306_WIDTH - len * SSD1306_CHAR_W)
                  : 0u;
    ssd1306_draw_string(page, col, str, false);
}

/* A thin horizontal rule at the given page (single pixel row). */
static void draw_list_item(uint8_t page, bool selected, const char *label)
{
    char buf[22];
    snprintf(buf, sizeof(buf), "%c%s", selected ? '>' : ' ', label);
    ssd1306_draw_string(page, 0, buf, false);
}

static void draw_list_item_kv(uint8_t page, bool selected,
                                const char *label, const char *value)
{
    char buf[22];
    int llen = (int)strlen(label);
    int vlen = (int)strlen(value);
    int spaces = 20 - llen - vlen;
    if (spaces < 1) spaces = 1;
    snprintf(buf, sizeof(buf), "%c%s%*s%s",
             selected ? '>' : ' ', label, spaces, "", value);
    ssd1306_draw_string(page, 0, buf, false);
}

static void draw_section_heading(const char *section_name)
{
    char heading[32];
    snprintf(heading, sizeof(heading), "Settings>%s", section_name);
    jpp_draw_title(heading);
}

static void draw_pagination(size_t current, size_t total)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    char buf[8];
    snprintf(buf, sizeof(buf), "%u/%u",
             (unsigned)(current + 1u), (unsigned)total);
#pragma GCC diagnostic pop
    draw_right(0u, buf);
}

/* ---- Duration helpers --------------------------------------------------- */

static void duration_label(char *buf, size_t bufsz, int32_t s)
{
    if (s <= 0)        snprintf(buf, bufsz, "Never");
    else if (s < 60)   snprintf(buf, bufsz, "%ds", (int)s);
    else               snprintf(buf, bufsz, "%dm", (int)(s / 60));
}

static const int32_t DURATION_STEPS[] = {0,5,10,15,30,60,120,300,600,1800};
#define DURATION_STEP_COUNT (sizeof(DURATION_STEPS)/sizeof(DURATION_STEPS[0]))

static int32_t next_duration(int32_t current)
{
    for (size_t i = 0u; i + 1u < DURATION_STEP_COUNT; i++) {
        if (current == DURATION_STEPS[i]) return DURATION_STEPS[i + 1u];
    }
    return DURATION_STEPS[0];
}

/* ---- Volume step helpers ------------------------------------------------ */

static const uint8_t VOLUME_STEPS[] = {0, 25, 50, 75, 100};
#define VOLUME_STEP_COUNT (sizeof(VOLUME_STEPS)/sizeof(VOLUME_STEPS[0]))

/* Return the index in VOLUME_STEPS that equals pct, or the last index. */
static size_t volume_step_index(uint8_t pct)
{
    for (size_t i = 0u; i < VOLUME_STEP_COUNT; i++) {
        if (VOLUME_STEPS[i] == pct) return i;
    }
    return VOLUME_STEP_COUNT - 1u;
}

/* ======================================================================== */
/* Section renderers                                                          */
/* ======================================================================== */

static const char *SECTION_NAMES[JPP_SETTINGS_SECTION_COUNT] = {
    "Shutdown/Reboot", "Personalisation", "Wi-Fi", "Time", "Sleep timers",
    "Sound", "SD card", "Backup settings", "Factory Reset",
    "* Device Info *", "Dummy Mode", "About",
};

/* Returns false for sections that should be hidden given the current state. */
static bool section_is_visible(jpp_settings_section_t s,
                                const jpp_settings_state_t *state)
{
    if (s == JPP_SETTINGS_SECTION_DEVICE_INFO) {
        return state->lrv_has_data;
    }
    return true;
}

/* Count of currently-visible sections. */
static size_t visible_section_count(const jpp_settings_state_t *state)
{
    size_t n = 0u;
    for (size_t i = 0u; i < JPP_SETTINGS_SECTION_COUNT; i++) {
        if (section_is_visible((jpp_settings_section_t)i, state)) { n++; }
    }
    return n;
}

/* Map a visible-index (0-based) to a jpp_settings_section_t. */
static jpp_settings_section_t visible_index_to_section(
    size_t visible_idx, const jpp_settings_state_t *state)
{
    size_t v = 0u;
    for (size_t i = 0u; i < JPP_SETTINGS_SECTION_COUNT; i++) {
        if (section_is_visible((jpp_settings_section_t)i, state)) {
            if (v == visible_idx) { return (jpp_settings_section_t)i; }
            v++;
        }
    }
    return JPP_SETTINGS_SECTION_ABOUT;
}

/* Map a section to its visible index, or SIZE_MAX if hidden. */
static size_t section_to_visible_index(jpp_settings_section_t s,
                                        const jpp_settings_state_t *state)
{
    size_t v = 0u;
    for (size_t i = 0u; i < JPP_SETTINGS_SECTION_COUNT; i++) {
        if ((jpp_settings_section_t)i == s) {
            return section_is_visible(s, state) ? v : (size_t)-1u;
        }
        if (section_is_visible((jpp_settings_section_t)i, state)) { v++; }
    }
    return (size_t)-1u;
}

static void render_top_level(jpp_settings_state_t *state)
{
    size_t n    = visible_section_count(state);
    size_t cur  = section_to_visible_index(state->selected_section, state);
    if (cur == (size_t)-1u) { cur = 0u; }

    jpp_draw_title("Settings");
    draw_pagination(cur, n);

    size_t start = cur > 2u ? cur - 2u : 0u;
    if (start + 5u > n) { start = (n >= 5u) ? n - 5u : 0u; }
    size_t end = (start + 5u < n) ? start + 5u : n;
    for (size_t v = start; v < end; v++) {
        jpp_settings_section_t s = visible_index_to_section(v, state);
        draw_list_item((uint8_t)(2u + v - start),
                       s == state->selected_section,
                       SECTION_NAMES[s]);
    }
}

static void render_wifi(const jpp_settings_state_t *state)
{
    draw_section_heading("Wi-Fi");
    switch (state->wifi_ss) {
    case JPP_WIFI_SS_MAIN: {
        bool connected = strncmp(state->wifi_status_msg, "Connected", 9) == 0;
        if (connected) {
            ssd1306_draw_string(2, 0, state->wifi_status_msg, false);
            draw_list_item(4, state->wifi_list_cursor == 0, "Disconnect");
            draw_list_item(5, state->wifi_list_cursor == 1, "Known networks");
        } else if (state->wifi_is_connecting) {
            ssd1306_draw_string(2, 0, "Connecting to:", false);
            if (state->wifi_saved_ssid[0] != '\0') {
                ssd1306_draw_string(3, 0, state->wifi_saved_ssid, false);
            }
            draw_list_item(5, true, "Cancel");
        } else {
            ssd1306_draw_string(2, 0, "Not connected", false);
            draw_list_item(4, state->wifi_list_cursor == 0, "Scan networks");
            draw_list_item(5, state->wifi_list_cursor == 1, "Known networks");
        }
        break;
    }
    case JPP_WIFI_SS_SCANNING:
        /* Render "Scanning..." and set pending flag — actual scan is triggered
           by jpp_settings_screen_render() after this frame has been flushed. */
        ssd1306_draw_string(2, 0, "Scanning...", false);
        ssd1306_draw_string(3, 0, ">             ", false); /* animated feel */
        break;
    case JPP_WIFI_SS_NETWORK_LIST: {
        ssd1306_draw_string(2, 0, "Select network:", false);
        if (state->wifi_scan_count == 0u) {
            ssd1306_draw_string(3, 0, "  (none found)", false);
        } else {
            size_t visible = 4u;
            size_t scroll  = jpp_ui_scroll_clamp(state->wifi_list_cursor,
                                                 state->wifi_scan_count,
                                                 visible,
                                                 state->wifi_list_scroll);
            /* Store updated scroll (const cast: render needs to update scroll). */
            ((jpp_settings_state_t *)state)->wifi_list_scroll = scroll;

            size_t end = scroll + visible;
            if (end > state->wifi_scan_count) { end = state->wifi_scan_count; }
            for (size_t i = scroll; i < end; i++) {
                char item[20];
                snprintf(item, sizeof(item), "%.18s", state->wifi_scan_results[i].ssid);
                draw_list_item((uint8_t)(3u + i - scroll),
                               i == state->wifi_list_cursor, item);
            }
            /* Scroll indicator: "3/8" in top-right */
            if (state->wifi_scan_count > visible) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
                char pager[8];
                snprintf(pager, sizeof(pager), "%u/%u",
                         (unsigned)(state->wifi_list_cursor + 1u),
                         (unsigned)state->wifi_scan_count);
#pragma GCC diagnostic pop
                draw_right(2u, pager);
            }
        }
        break;
    }
    case JPP_WIFI_SS_CONNECTING:
        ssd1306_draw_string(2, 0, "Connecting to:", false);
        ssd1306_draw_string(3, 0, state->wifi_connecting_ssid, false);
        if (state->wifi_status_msg[0] != '\0') {
            ssd1306_draw_string(5, 0, state->wifi_status_msg, false);
        }
        break;
    case JPP_WIFI_SS_KNOWN_NETWORKS:
        ssd1306_draw_string(2, 0, "Known networks:", false);
        if (state->wifi_saved_ssid[0] != '\0') {
            char known[22];
            snprintf(known, sizeof(known), " %.19s", state->wifi_saved_ssid);
            ssd1306_draw_string(3, 0, known, false);
        } else {
            ssd1306_draw_string(3, 0, " (none saved)", false);
        }
        break;
    default: break;
    }
}

static void render_time(const jpp_settings_state_t *state,
                         const jpp_settings_deps_t *deps)
{
    draw_section_heading("Time");
    jpp_rtc_datetime_t now = {0};
    bool has_time = false;
    if (deps->rtc != NULL) {
        has_time = (jpp_rtc_get_current(deps->rtc, &now) == JPP_RTC_STATUS_OK);
    }

    switch (state->time_ss) {
    case JPP_TIME_SS_MAIN: {
        char buf[22];
        if (has_time) {
            snprintf(buf, sizeof(buf), "%02d.%02d.%04d %02d:%02d:%02d",
                     now.day, now.month, now.year,
                     now.hour, now.minute, now.second);
            ssd1306_draw_string(2, 0, buf, false);
        } else {
            ssd1306_draw_string(2, 0, "Time not set", false);
        }
        snprintf(buf, sizeof(buf), "NTP: %s",
                 state->ntp_enabled_staging ? "ON" : "OFF");
        ssd1306_draw_string(4, 0, buf, false);
        ssd1306_draw_string(6, 0, "OK to configure", false);
        break;
    }
    case JPP_TIME_SS_LIST: {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        char tz_label[10];
        if (state->timezone_offset_h >= 0)
            snprintf(tz_label, sizeof(tz_label), "UTC+%d", state->timezone_offset_h);
        else
            snprintf(tz_label, sizeof(tz_label), "UTC%d",  state->timezone_offset_h);
#pragma GCC diagnostic pop
        draw_list_item_kv(2, state->time_list_cursor == 0, "NTP sync",
                          state->ntp_enabled_staging ? "ON" : "OFF");
        draw_list_item_kv(3, state->time_list_cursor == 1, "NTP server",
                          state->ntp_host_staging[0] ? state->ntp_host_staging
                                                      : "time.nist.gov");
        draw_list_item_kv(4, state->time_list_cursor == 2, "Timezone", tz_label);
        break;
    }
    case JPP_TIME_SS_TIMEZONE: {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        char tz_buf[12];
        if (state->timezone_offset_h >= 0)
            snprintf(tz_buf, sizeof(tz_buf), "UTC+%d", state->timezone_offset_h);
        else
            snprintf(tz_buf, sizeof(tz_buf), "UTC%d",  state->timezone_offset_h);
#pragma GCC diagnostic pop
        ssd1306_draw_string(2, 0, "Timezone offset:", false);
        ssd1306_draw_string(3, 0, tz_buf, false);
        ssd1306_draw_string(5, 0, "UP/DOWN to change", false);
        ssd1306_draw_string(6, 0, "OK to save", false);
        break;
    }
    default: break;
    }
}

static void format_size(char *buf, size_t bufsz, uint64_t bytes)
{
    if (bytes >= (uint64_t)1024 * 1024 * 1024) {
        uint32_t u = (uint32_t)(bytes / ((uint64_t)1024 * 1024 * 1024));
        uint32_t f = (uint32_t)((bytes % ((uint64_t)1024 * 1024 * 1024)) * 10
                                / ((uint64_t)1024 * 1024 * 1024));
        snprintf(buf, bufsz, "%lu.%lu GB", (unsigned long)u, (unsigned long)f);
    } else if (bytes >= (uint64_t)1024 * 1024) {
        uint32_t u = (uint32_t)(bytes / ((uint64_t)1024 * 1024));
        uint32_t f = (uint32_t)((bytes % ((uint64_t)1024 * 1024)) * 10
                                / ((uint64_t)1024 * 1024));
        snprintf(buf, bufsz, "%lu.%lu MB", (unsigned long)u, (unsigned long)f);
    } else {
        snprintf(buf, bufsz, "%lu KB", (unsigned long)(bytes / 1024));
    }
}

static const char *sd_spec_ver(uint32_t s)
{
    if (s == 0) return "1.0";
    if (s == 1) return "1.1";
    if (s == 2) return "2.0+";
    return "3.0+";
}

static void render_storage(const jpp_settings_deps_t *deps)
{
    draw_section_heading("SD card");
    if (deps->sd_card_ptr == NULL || *deps->sd_card_ptr == NULL) {
        ssd1306_draw_string(3, 0, "No SD card", false);
        return;
    }
    sdmmc_card_t *card = *deps->sd_card_ptr;
    char buf[22];
    char sz[12];

    /* Free / Used / Total */
    uint64_t total_b = 0, free_b = 0;
    if (esp_vfs_fat_info("/sd", &total_b, &free_b) == ESP_OK) {
        uint64_t used_b = (total_b >= free_b) ? (total_b - free_b) : 0;
        format_size(sz, sizeof(sz), free_b);
        snprintf(buf, sizeof(buf), "Free:  %s", sz);
        ssd1306_draw_string(2, 0, buf, false);
        format_size(sz, sizeof(sz), used_b);
        snprintf(buf, sizeof(buf), "Used:  %s", sz);
        ssd1306_draw_string(3, 0, buf, false);
        format_size(sz, sizeof(sz), total_b);
        snprintf(buf, sizeof(buf), "Total: %s", sz);
        ssd1306_draw_string(4, 0, buf, false);
    } else {
        ssd1306_draw_string(2, 0, "Size unavailable", false);
    }

    /* Serial number */
    snprintf(buf, sizeof(buf), "S/N: %08lX", (unsigned long)card->cid.serial);
    ssd1306_draw_string(5, 0, buf, false);

    /* Manufacture date (MDT bits[11:4]=year offset from 2000, bits[3:0]=month)
       and SD spec version from SCR. */
    int year  = 2000 + ((card->cid.date >> 4) & 0xFF);
    int month = card->cid.date & 0xF;
    if (!card->is_mmc) {
        snprintf(buf, sizeof(buf), "%d-%02d  SD %s", year, month,
                 sd_spec_ver(card->scr.sd_spec));
    } else {
        snprintf(buf, sizeof(buf), "%d-%02d  MMC", year, month);
    }
    ssd1306_draw_string(6, 0, buf, false);

    /* FAT volume label */
    char label[25] = "";
    FRESULT fr = f_getlabel("0:", label, NULL);
    if (fr != FR_OK || label[0] == '\0') {
        snprintf(buf, sizeof(buf), "Label: (none)");
    } else {
        snprintf(buf, sizeof(buf), "Label: %.13s", label);
    }
    ssd1306_draw_string(7, 0, buf, false);
}

static void render_sleep_timers_section(const jpp_settings_state_t *state,
                                        const jpp_settings_deps_t *deps)
{
    draw_section_heading("Sleep timers");
    char dim_val[10], poweroff_val[10];
    int32_t sb = deps->shell ? deps->shell->dim_time_s      : 60;
    int32_t sl = deps->shell ? deps->shell->poweroff_time_s : 300;
    duration_label(dim_val,      sizeof(dim_val),      sb);
    duration_label(poweroff_val, sizeof(poweroff_val), sl);
    draw_list_item_kv(2, state->sleep_timers_cursor == 0, "Dim screen", dim_val);
    draw_list_item_kv(3, state->sleep_timers_cursor == 1, "Power off",  poweroff_val);
}

static void render_sound(const jpp_settings_state_t *state)
{
    draw_section_heading("Sound");
    char vol_label[6];
    if (state->sound_volume_pct == 0u)
        snprintf(vol_label, sizeof(vol_label), "OFF");
    else
        snprintf(vol_label, sizeof(vol_label), "%u%%", state->sound_volume_pct);
    draw_list_item_kv(2, state->sound_cursor == 0u, "Volume", vol_label);
    const char *jname = jpp_startup_jingle_name((jpp_startup_jingle_t)state->sound_jingle);
    draw_list_item_kv(3, state->sound_cursor == 1u, "Jingle", jname);
    draw_list_item   (4, state->sound_cursor == 2u, "Test");
    ssd1306_draw_string(6, 0, "L/R: change", false);
    ssd1306_draw_string(7, 0, "OK on Test: play", false);
}

static void render_personalisation(const jpp_settings_state_t *state)
{
    draw_section_heading("Personalisation");

    /* Labels are all 11 chars so ">label <value>" fills the 21-char row
       exactly with values of up to 8 chars. */
    const char *back_label = state->back_gesture_mode ? "2x Click" : "Hold";
    draw_list_item_kv(2, state->personalisation_cursor == 0u,
                      "Back action", back_label);

    char uname[9];
    snprintf(uname, sizeof(uname), "%.8s",
             state->username_current[0] != '\0' ? state->username_current : "-");
    draw_list_item_kv(3, state->personalisation_cursor == 1u,
                      "User's name", uname);

    draw_list_item_kv(4, state->personalisation_cursor == 2u,
                      "System apps", state->system_apps_bottom ? "bottom" : "top");

    ssd1306_draw_string(6, 0, "L/R: change", false);
    ssd1306_draw_string(7, 0, "OK on name: edit", false);
}

/* ---- Shutdown/Reboot 32×32 icons --------------------------------------- */

static const uint8_t ICON_REBOOT_FOCUS[128] = {
    0x7f, 0xff, 0xff, 0xfe,
    0xff, 0xff, 0xff, 0xff,
    0xe0, 0x00, 0x00, 0x07,
    0xc0, 0x00, 0x00, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0x81, 0x81, 0x03,
    0xc1, 0xc1, 0x83, 0x83,
    0xc0, 0xe1, 0x87, 0x03,
    0xc0, 0x71, 0x8e, 0x03,
    0xc0, 0x38, 0x1c, 0x03,
    0xc0, 0x10, 0x08, 0x03,
    0xc0, 0x00, 0x00, 0x03,
    0xcf, 0xe0, 0x07, 0xf3,
    0xcf, 0xe0, 0x07, 0xf3,
    0xc0, 0x00, 0x00, 0x03,
    0xc0, 0x00, 0x00, 0x03,
    0xc0, 0x10, 0x08, 0x03,
    0xc0, 0x38, 0x1c, 0x03,
    0xc0, 0x70, 0x0e, 0x03,
    0xc0, 0xe1, 0x87, 0x03,
    0xc1, 0xc1, 0x83, 0x83,
    0xc0, 0x81, 0x81, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0x00, 0x00, 0x03,
    0xe0, 0x00, 0x00, 0x07,
    0xff, 0xff, 0xff, 0xff,
    0x7f, 0xff, 0xff, 0xfe,
    0x00, 0x00, 0x00, 0x00,
};

static const uint8_t ICON_REBOOT_DEFAULT[128] = {
    0x00, 0x00, 0x00, 0x00,
    0x1f, 0xff, 0xff, 0xf8,
    0x20, 0x00, 0x00, 0x04,
    0x40, 0x00, 0x00, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0x81, 0x81, 0x02,
    0x41, 0xc1, 0x83, 0x82,
    0x40, 0xe1, 0x87, 0x02,
    0x40, 0x71, 0x8e, 0x02,
    0x40, 0x38, 0x1c, 0x02,
    0x40, 0x10, 0x08, 0x02,
    0x40, 0x00, 0x00, 0x02,
    0x40, 0x00, 0x00, 0x02,
    0x47, 0xe0, 0x07, 0xe2,
    0x47, 0xe0, 0x07, 0xe2,
    0x40, 0x00, 0x00, 0x02,
    0x40, 0x00, 0x00, 0x02,
    0x40, 0x10, 0x08, 0x02,
    0x40, 0x38, 0x1c, 0x02,
    0x40, 0x70, 0x0e, 0x02,
    0x40, 0xe1, 0x87, 0x02,
    0x41, 0xc1, 0x83, 0x82,
    0x40, 0x81, 0x81, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0x00, 0x00, 0x02,
    0x20, 0x00, 0x00, 0x04,
    0x1f, 0xff, 0xff, 0xf8,
    0x00, 0x00, 0x00, 0x00,
};

static const uint8_t ICON_SHUTDOWN_FOCUS[128] = {
    0x7f, 0xff, 0xff, 0xfe,
    0xff, 0xff, 0xff, 0xff,
    0xe0, 0x00, 0x00, 0x07,
    0xc0, 0x00, 0x00, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0x01, 0x80, 0x03,
    0xc0, 0xc1, 0x83, 0x03,
    0xc1, 0xc1, 0x83, 0x83,
    0xc1, 0x81, 0x81, 0x83,
    0xc1, 0x81, 0x81, 0x83,
    0xc3, 0x01, 0x80, 0xc3,
    0xc3, 0x01, 0x80, 0xc3,
    0xc3, 0x01, 0x80, 0xc3,
    0xc3, 0x01, 0x80, 0xc3,
    0xc3, 0x00, 0x00, 0xc3,
    0xc3, 0x00, 0x00, 0xc3,
    0xc1, 0x80, 0x01, 0x83,
    0xc1, 0x80, 0x01, 0x83,
    0xc0, 0xc0, 0x03, 0x03,
    0xc0, 0xe0, 0x07, 0x03,
    0xc0, 0x78, 0x1e, 0x03,
    0xc0, 0x1f, 0xf8, 0x03,
    0xc0, 0x07, 0xe0, 0x03,
    0xc0, 0x00, 0x00, 0x03,
    0xe0, 0x00, 0x00, 0x07,
    0xff, 0xff, 0xff, 0xff,
    0x7f, 0xff, 0xff, 0xfe,
};

static const uint8_t ICON_SHUTDOWN_DEFAULT[128] = {
    0x00, 0x00, 0x00, 0x00,
    0x1f, 0xff, 0xff, 0xf8,
    0x20, 0x00, 0x00, 0x04,
    0x40, 0x00, 0x00, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0x01, 0x80, 0x02,
    0x40, 0xc1, 0x83, 0x02,
    0x41, 0xc1, 0x83, 0x82,
    0x41, 0x81, 0x81, 0x82,
    0x41, 0x81, 0x81, 0x82,
    0x43, 0x01, 0x80, 0xc2,
    0x43, 0x01, 0x80, 0xc2,
    0x43, 0x01, 0x80, 0xc2,
    0x43, 0x01, 0x80, 0xc2,
    0x43, 0x00, 0x00, 0xc2,
    0x43, 0x00, 0x00, 0xc2,
    0x41, 0x80, 0x01, 0x82,
    0x41, 0x80, 0x01, 0x82,
    0x40, 0xc0, 0x03, 0x02,
    0x40, 0xe0, 0x07, 0x02,
    0x40, 0x78, 0x1e, 0x02,
    0x40, 0x1f, 0xf8, 0x02,
    0x40, 0x07, 0xe0, 0x02,
    0x40, 0x00, 0x00, 0x02,
    0x20, 0x00, 0x00, 0x04,
    0x1f, 0xff, 0xff, 0xf8,
    0x00, 0x00, 0x00, 0x00,
};

static void render_shutdown_reboot(const jpp_settings_state_t *state)
{
    draw_section_heading("Shutdown/Reboot");

    bool reboot_focused  = (state->shutdown_reboot_cursor == 0);
    bool shutdown_focused = (state->shutdown_reboot_cursor == 1);

    ssd1306_draw_bitmap_32x32(2, 16,
        reboot_focused ? ICON_REBOOT_FOCUS : ICON_REBOOT_DEFAULT, false);
    ssd1306_draw_bitmap_32x32(2, 80,
        shutdown_focused ? ICON_SHUTDOWN_FOCUS : ICON_SHUTDOWN_DEFAULT, false);

    ssd1306_draw_string(6, 14, "Reboot", reboot_focused);
    ssd1306_draw_string(6, 72, "Shutdown", shutdown_focused);
}


static void render_backup_settings(const jpp_settings_state_t *state)
{
    draw_section_heading("Backup settings");
    switch (state->backup_ss) {
    case JPP_BACKUP_SS_MAIN:
        draw_list_item(2, state->backup_cursor == 0, "Backup to SD card");
        draw_list_item(3, state->backup_cursor == 1, "Restore from file");
        break;
    case JPP_BACKUP_SS_RESULT:
        /* Show up to two lines of the result message (split at '\n' if present). */
        {
            const char *msg = state->backup_result_msg;
            const char *nl  = strchr(msg, '\n');
            if (nl == NULL) {
                ssd1306_draw_string(3, 0, msg, false);
            } else {
                char line1[22];
                size_t l = (size_t)(nl - msg);
                if (l >= sizeof(line1)) { l = sizeof(line1) - 1u; }
                memcpy(line1, msg, l);
                line1[l] = '\0';
                ssd1306_draw_string(3, 0, line1, false);
                ssd1306_draw_string(4, 0, nl + 1, false);
            }
            ssd1306_draw_string(6, 0, "OK to go back", false);
        }
        break;
    default: break;
    }
}

static void render_device_info(const jpp_settings_state_t *state)
{
    draw_section_heading("Device Info");
    switch (state->lrv_ss) {
    case JPP_LRV_SS_MAIN: {
        draw_centred(2, "J++Device");
        char unit[20];
        snprintf(unit, sizeof(unit), "Unit #%u", (unsigned)state->lrv_serial);
        draw_centred(3, unit);
        char pk[24];
        snprintf(pk, sizeof(pk), "PubKey: %.13s", state->lrv_pubkey_str);
        ssd1306_draw_string(5, 0, pk, false);
        ssd1306_draw_string(6, 0, "OK to verify", false);
        break;
    }
    case JPP_LRV_SS_VERIFY_RESULT:
        ssd1306_draw_string(2, 0, "Printed certificate", false);
        ssd1306_draw_string(3, 0, "to serial.", false);
        if (state->lrv_server_running) {
            ssd1306_draw_string(5, 0, "Interactive verify at", false);
            ssd1306_draw_string(6, 0, state->lrv_server_addr, false);
        } else {
            ssd1306_draw_string(5, 0, "Interactive verify", false);
            ssd1306_draw_string(6, 0, "available with Wi-Fi.", false);
        }
        break;
    case JPP_LRV_SS_VERIFY_ERROR:
        ssd1306_draw_string(2, 0, "Cannot start server:", false);
        ssd1306_draw_string(3, 0, state->lrv_verify_error, false);
        ssd1306_draw_string(6, 0, "Any key to go back", false);
        break;
    default: break;
    }
}

static void render_dummy_mode(const jpp_settings_state_t *state,
                               const jpp_settings_deps_t *deps)
{
    draw_section_heading("Dummy Mode");

    if (state->dummy_enabled) {
        ssd1306_draw_string(2, 0, "ENABLED", false);
        char app_line[22];
        snprintf(app_line, sizeof(app_line), "App: %.16s",
                 state->dummy_app_name[0] ? state->dummy_app_name
                                          : state->dummy_app_id);
        ssd1306_draw_string(3, 0, app_line, false);
        ssd1306_draw_string(5, 0, "Hold OK on boot", false);
        ssd1306_draw_string(6, 0, "to disable.", false);
        return;
    }

    ssd1306_draw_string(2, 0, "Locks to 1 app.", false);
    ssd1306_draw_string(3, 0, "Disable:hold OK+boot", false);

    const jpp_ui_shell_t *sh = deps->shell;
    size_t sd_count = 0;
    for (size_t i = 0; i < sh->app_count; i++) {
        if (sh->apps[i].source == JPP_UI_APP_SOURCE_SD) { sd_count++; }
    }

    if (sd_count == 0) {
        ssd1306_draw_string(5, 0, "No apps on SD card.", false);
        ssd1306_draw_string(6, 0, "BACK to close.", false);
        return;
    }

    /* App list: 4 visible rows (pages 4-7), scrollable. */
    const size_t visible = 4u;
    size_t scroll = jpp_ui_scroll_clamp(state->dummy_cursor, sd_count,
                                         visible, state->dummy_scroll);
    ((jpp_settings_state_t *)state)->dummy_scroll = scroll;

    size_t vi = 0, shown = 0;
    for (size_t i = 0; i < sh->app_count && shown < visible; i++) {
        if (sh->apps[i].source != JPP_UI_APP_SOURCE_SD) { continue; }
        if (vi < scroll) { vi++; continue; }
        char item[20];
        snprintf(item, sizeof(item), "%.19s", sh->apps[i].name);
        draw_list_item((uint8_t)(4u + shown), vi == state->dummy_cursor, item);
        vi++;
        shown++;
    }

    if (sd_count > visible) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        char pager[8];
        snprintf(pager, sizeof(pager), "%u/%u",
                 (unsigned)(state->dummy_cursor + 1u),
                 (unsigned)sd_count);
#pragma GCC diagnostic pop
        draw_right(3u, pager);
    }
}

static bool handle_dummy_mode(jpp_settings_state_t *state,
                               const jpp_settings_deps_t *deps,
                               jpp_ui_action_t action)
{
    if (state->dummy_enabled) {
        /* Already enabled: any key closes section (user reads the disable hint). */
        return (action == JPP_UI_ACTION_BACK || action == JPP_UI_ACTION_OK);
    }

    const jpp_ui_shell_t *sh = deps->shell;
    size_t sd_count = 0;
    for (size_t i = 0; i < sh->app_count; i++) {
        if (sh->apps[i].source == JPP_UI_APP_SOURCE_SD) { sd_count++; }
    }

    if (sd_count == 0) {
        return (action == JPP_UI_ACTION_BACK);
    }

    switch (action) {
    case JPP_UI_ACTION_UP:
        if (state->dummy_cursor > 0) { state->dummy_cursor--; }
        break;
    case JPP_UI_ACTION_DOWN:
        if (state->dummy_cursor + 1u < sd_count) { state->dummy_cursor++; }
        break;
    case JPP_UI_ACTION_OK: {
        /* Find the SD app at dummy_cursor and enable dummy mode. */
        size_t vi = 0;
        for (size_t i = 0; i < sh->app_count; i++) {
            if (sh->apps[i].source != JPP_UI_APP_SOURCE_SD) { continue; }
            if (vi == state->dummy_cursor) {
                strncpy(state->dummy_app_id, sh->apps[i].app_id,
                        sizeof(state->dummy_app_id) - 1u);
                state->dummy_app_id[sizeof(state->dummy_app_id) - 1u] = '\0';
                strncpy(state->dummy_app_name, sh->apps[i].name,
                        sizeof(state->dummy_app_name) - 1u);
                state->dummy_app_name[sizeof(state->dummy_app_name) - 1u] = '\0';
                break;
            }
            vi++;
        }
        state->dummy_enabled = true;
        if (deps->do_dummy_mode_save) {
            deps->do_dummy_mode_save(true, state->dummy_app_id);
        }
        /* Restart device to activate dummy mode immediately. */
        cls();
        draw_section_heading("Dummy Mode");
        ssd1306_draw_string(3, 0, "ENABLED", false);
        char app_line[22];
        snprintf(app_line, sizeof(app_line), "%.21s", state->dummy_app_name);
        ssd1306_draw_string(4, 0, app_line, false);
        ssd1306_draw_string(6, 0, "Restarting...", false);
        ssd1306_flush();
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (deps->do_reboot) { deps->do_reboot(); }
        break;
    }
    case JPP_UI_ACTION_BACK:
        return true;
    default:
        break;
    }
    return false;
}

static void render_about(void)
{
    /* The device URL is wider than the 21-char (128px/6px) row, so scroll it
       as a horizontal marquee. https:// prefix and trailing / signal a link. */
    static const char url[] = "https://jppdevice.by.m4l3vi.ch/";
    static const size_t url_len = sizeof(url) - 1u;
    static const size_t window  = SSD1306_WIDTH / SSD1306_CHAR_W;   /* 21 chars */
    static uint32_t marquee = 0u;

    draw_centred_2x(0, "JPPDOS");
    draw_centred(2, "v" JPPDOS_VERSION);
    jpp_draw_rule(3u);
    draw_centred(4, "Open & hackable fw");
    draw_centred(5, "by Claude & m4l3vich");

    size_t off = jpp_ui_marquee_offset(marquee, url_len, window, 6u);
    ssd1306_draw_string(7, 0, url + off, false);
    marquee++;
}

static void render_factory_reset(const jpp_settings_state_t *state)
{
    draw_section_heading("Factory Reset");
    ssd1306_draw_string(2, 0, "Erase all settings", false);
    ssd1306_draw_string(3, 0, "and grants.", false);
    draw_list_item(5, state->factory_reset_cursor == 0, "Cancel");
    draw_list_item(6, state->factory_reset_cursor == 1, "Reset now");
}

/* ======================================================================== */
/* Public entry points                                                         */
/* ======================================================================== */

void jpp_settings_screen_init(jpp_settings_state_t *state,
                               const jpp_settings_deps_t *deps)
{
    (void)deps;
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
}

void jpp_settings_screen_render(jpp_settings_state_t *state,
                                 const jpp_settings_deps_t *deps)
{
    if (state == NULL || deps == NULL) return;

    /* Keep Wi-Fi connecting state current on every render tick so the main
       subscreen reflects background auto-reconnect changes without delay. */
    if (state->section_open &&
        state->selected_section == JPP_SETTINGS_SECTION_WIFI &&
        state->wifi_ss == JPP_WIFI_SS_MAIN &&
        deps->do_wifi_check_status) {
        deps->do_wifi_check_status(state);
    }

    cls();
    if (!state->section_open) {
        render_top_level(state);
    } else {
        switch (state->selected_section) {
        case JPP_SETTINGS_SECTION_SHUTDOWN_REBOOT: render_shutdown_reboot(state); break;
        case JPP_SETTINGS_SECTION_PERSONALISATION: render_personalisation(state); break;
        case JPP_SETTINGS_SECTION_WIFI:          render_wifi(state);             break;
        case JPP_SETTINGS_SECTION_TIME:          render_time(state, deps);       break;
        case JPP_SETTINGS_SECTION_SLEEP_TIMERS:  render_sleep_timers_section(state, deps); break;
        case JPP_SETTINGS_SECTION_SOUND:         render_sound(state);                  break;
        case JPP_SETTINGS_SECTION_SD_CARD:       render_storage(deps);                 break;
        case JPP_SETTINGS_SECTION_BACKUP:        render_backup_settings(state);        break;
        case JPP_SETTINGS_SECTION_FACTORY_RESET: render_factory_reset(state);          break;
        case JPP_SETTINGS_SECTION_DEVICE_INFO:   render_device_info(state);            break;
        case JPP_SETTINGS_SECTION_DUMMY_MODE:   render_dummy_mode(state, deps);       break;
        case JPP_SETTINGS_SECTION_ABOUT:         render_about();                       break;
        default: break;
        }
    }
    ssd1306_flush();

    /* If a Wi-Fi scan was requested, the "Scanning..." screen is now on the
       display.  Run the blocking scan, then immediately re-render the results. */
    if (state->section_open &&
        state->selected_section == JPP_SETTINGS_SECTION_WIFI &&
        state->wifi_ss == JPP_WIFI_SS_SCANNING &&
        state->wifi_scan_pending) {
        state->wifi_scan_pending = false;
        if (deps->do_wifi_scan) { deps->do_wifi_scan(state); }
        state->wifi_ss = JPP_WIFI_SS_NETWORK_LIST;
        /* Re-render immediately so the user sees results without a tick delay. */
        cls();
        render_wifi(state);
        ssd1306_flush();
    }

    /* If a blocking Wi-Fi connect was requested, the "Connecting to: <ssid>"
       screen is now on the display.  Run the connect, update status, re-render. */
    if (state->section_open &&
        state->selected_section == JPP_SETTINGS_SECTION_WIFI &&
        state->wifi_ss == JPP_WIFI_SS_CONNECTING &&
        state->wifi_connect_pending) {
        state->wifi_connect_pending = false;
        if (deps->do_wifi_connect) {
            deps->do_wifi_connect(state,
                                   state->wifi_connecting_ssid,
                                   state->wifi_pending_password);
        }
        /* do_wifi_connect updates wifi_status_msg; go back to main view. */
        state->wifi_ss = JPP_WIFI_SS_MAIN;
        cls();
        render_wifi(state);
        ssd1306_flush();
    }
}

/* ---- Action handlers ---------------------------------------------------- */

static bool handle_top_level(jpp_settings_state_t *state,
                              const jpp_settings_deps_t *deps,
                              jpp_ui_action_t action)
{
    size_t n   = visible_section_count(state);
    size_t cur = section_to_visible_index(state->selected_section, state);
    if (cur == (size_t)-1u) { cur = 0u; }

    switch (action) {
    case JPP_UI_ACTION_UP:
        if (cur > 0u) {
            state->selected_section = visible_index_to_section(cur - 1u, state);
        }
        break;
    case JPP_UI_ACTION_DOWN:
        if (cur + 1u < n) {
            state->selected_section = visible_index_to_section(cur + 1u, state);
        }
        break;
    case JPP_UI_ACTION_OK:
        state->section_open = true;
        state->wifi_list_cursor = state->time_list_cursor = 0;
        state->factory_reset_cursor = state->sleep_timers_cursor = 0;
        state->shutdown_reboot_cursor = 0;
        state->sound_cursor = 0;
        state->personalisation_cursor = 0;
        state->backup_cursor = 0;
        state->backup_ss     = JPP_BACKUP_SS_MAIN;
        state->wifi_ss = JPP_WIFI_SS_MAIN;
        state->time_ss = JPP_TIME_SS_MAIN;
        /* Initialise Device Info subscreen state on open. */
        if (state->selected_section == JPP_SETTINGS_SECTION_DEVICE_INFO) {
            state->lrv_ss = JPP_LRV_SS_MAIN;
            state->lrv_verify_error[0] = '\0';
        }
        /* Refresh Wi-Fi connection status so the MAIN screen shows the correct
           "Connected" / "Not connected" state immediately on open. */
        if (state->selected_section == JPP_SETTINGS_SECTION_WIFI &&
            deps && deps->do_wifi_check_status) {
            deps->do_wifi_check_status(state);
        }
        /* Dummy Mode: reset cursor and look up current app name on section open. */
        if (state->selected_section == JPP_SETTINGS_SECTION_DUMMY_MODE) {
            state->dummy_cursor = 0;
            state->dummy_scroll = 0;
            if (state->dummy_enabled && state->dummy_app_id[0] != '\0' &&
                deps && deps->shell) {
                state->dummy_app_name[0] = '\0';
                for (size_t i = 0; i < deps->shell->app_count; i++) {
                    if (strcmp(deps->shell->apps[i].app_id,
                               state->dummy_app_id) == 0) {
                        strncpy(state->dummy_app_name,
                                deps->shell->apps[i].name,
                                sizeof(state->dummy_app_name) - 1u);
                        state->dummy_app_name[sizeof(state->dummy_app_name)-1u]
                            = '\0';
                        break;
                    }
                }
            }
        }
        break;
    case JPP_UI_ACTION_BACK:
        return true;
    default: break;
    }
    return false;
}

static bool handle_wifi(jpp_settings_state_t *state,
                         const jpp_settings_deps_t *deps,
                         jpp_ui_action_t action)
{
    switch (state->wifi_ss) {
    case JPP_WIFI_SS_MAIN: {
        bool connected = strncmp(state->wifi_status_msg, "Connected", 9u) == 0;
        switch (action) {
        case JPP_UI_ACTION_UP:
            if (!state->wifi_is_connecting && state->wifi_list_cursor > 0u) {
                state->wifi_list_cursor--;
            }
            break;
        case JPP_UI_ACTION_DOWN:
            if (!state->wifi_is_connecting && state->wifi_list_cursor < 1u) {
                state->wifi_list_cursor++;
            }
            break;
        case JPP_UI_ACTION_OK:
            if (state->wifi_is_connecting) {
                /* "Cancel" — abort the auto-reconnect loop. */
                if (deps->do_wifi_disconnect) deps->do_wifi_disconnect(state);
            } else if (state->wifi_list_cursor == 0) {
                if (connected) {
                    /* Top item = "Disconnect" when connected. */
                    if (deps->do_wifi_disconnect) deps->do_wifi_disconnect(state);
                } else {
                    /* Top item = "Scan networks" when not connected.
                       Defer actual scan until after "Scanning..." is flushed. */
                    state->wifi_ss = JPP_WIFI_SS_SCANNING;
                    state->wifi_scan_pending = true;
                    state->wifi_list_cursor  = 0;
                    state->wifi_list_scroll  = 0;
                }
            } else {
                state->wifi_ss = JPP_WIFI_SS_KNOWN_NETWORKS;
            }
            break;
        case JPP_UI_ACTION_BACK: return true;
        default: break;
        }
        break;
    }
    case JPP_WIFI_SS_NETWORK_LIST:
        switch (action) {
        case JPP_UI_ACTION_UP:
            if (state->wifi_list_cursor > 0u) { state->wifi_list_cursor--; } break;
        case JPP_UI_ACTION_DOWN:
            if (state->wifi_list_cursor + 1u < state->wifi_scan_count) {
                state->wifi_list_cursor++;
            }
            break;
        case JPP_UI_ACTION_OK:
            if (state->wifi_list_cursor < state->wifi_scan_count) {
                const char *ssid = state->wifi_scan_results[state->wifi_list_cursor].ssid;
                strncpy(state->wifi_connecting_ssid, ssid,
                        sizeof(state->wifi_connecting_ssid) - 1u);
                state->wifi_connecting_ssid[sizeof(state->wifi_connecting_ssid)-1u] = '\0';

                /* Prompt for password now (keyboard is shown before connect screen). */
                state->wifi_pending_password[0] = '\0';
                if (state->wifi_scan_results[state->wifi_list_cursor].has_password &&
                    deps->do_text_input) {
                    bool ok = deps->do_text_input("Wi-Fi password", NULL,
                                                   JPP_KBD_TYPE_TEXT,
                                                   state->wifi_pending_password,
                                                   sizeof(state->wifi_pending_password));
                    if (!ok) break;   /* user cancelled password entry */
                }

                /* Switch to CONNECTING state.  The actual blocking connect is
                   deferred until jpp_settings_screen_render() has flushed the
                   "Connecting to: <ssid>" screen to the display. */
                state->wifi_ss            = JPP_WIFI_SS_CONNECTING;
                state->wifi_connect_pending = true;
                state->wifi_status_msg[0] = '\0';
            }
            break;
        case JPP_UI_ACTION_BACK:
            state->wifi_ss = JPP_WIFI_SS_MAIN;
            break;
        default: break;
        }
        break;
    case JPP_WIFI_SS_CONNECTING:
        /* Ignore input while connect is in progress (pending flag set).
           After connect completes the pending flag is cleared and state changes. */
        if (!state->wifi_connect_pending) {
            if (action == JPP_UI_ACTION_OK || action == JPP_UI_ACTION_BACK)
                state->wifi_ss = JPP_WIFI_SS_MAIN;
        }
        break;
    default:
        if (action == JPP_UI_ACTION_BACK) state->wifi_ss = JPP_WIFI_SS_MAIN;
        break;
    }
    return false;
}

static bool handle_time(jpp_settings_state_t *state,
                         const jpp_settings_deps_t *deps,
                         jpp_ui_action_t action)
{
    switch (state->time_ss) {
    case JPP_TIME_SS_MAIN:
        if (action == JPP_UI_ACTION_OK) {
            state->time_ss = JPP_TIME_SS_LIST;
            state->time_list_cursor = 0;
        } else if (action == JPP_UI_ACTION_BACK) {
            return true;
        }
        break;

    case JPP_TIME_SS_LIST:
        switch (action) {
        case JPP_UI_ACTION_UP:   if (state->time_list_cursor > 0u) { state->time_list_cursor--; } break;
        case JPP_UI_ACTION_DOWN: if (state->time_list_cursor < 2u) { state->time_list_cursor++; } break;
        case JPP_UI_ACTION_OK:
            if (state->time_list_cursor == 0) {
                /* Toggle NTP sync, save immediately. */
                state->ntp_enabled_staging = !state->ntp_enabled_staging;
                if (deps->do_ntp_save) {
                    deps->do_ntp_save(state->ntp_enabled_staging,
                                      state->ntp_host_staging,
                                      state->timezone_offset_h);
                }
            } else if (state->time_list_cursor == 1) {
                /* NTP server — blocking keyboard input. */
                if (deps->do_text_input) {
                    char buf[JPP_SETTINGS_NTP_HOST_MAX] = {0};
                    const char *prefill = state->ntp_host_staging[0]
                                         ? state->ntp_host_staging : NULL;
                    bool ok = deps->do_text_input("NTP server", prefill,
                                                   JPP_KBD_TYPE_TEXT,
                                                   buf, sizeof(buf));
                    if (ok && buf[0] != '\0') {
                        strncpy(state->ntp_host_staging, buf,
                                sizeof(state->ntp_host_staging) - 1u);
                        state->ntp_host_staging[sizeof(state->ntp_host_staging)-1u] = '\0';
                        if (deps->do_ntp_save) {
                            deps->do_ntp_save(state->ntp_enabled_staging,
                                              state->ntp_host_staging,
                                              state->timezone_offset_h);
                        }
                    }
                }
            } else {
                state->time_ss = JPP_TIME_SS_TIMEZONE;
            }
            break;
        case JPP_UI_ACTION_BACK: state->time_ss = JPP_TIME_SS_MAIN; break;
        default: break;
        }
        break;

    case JPP_TIME_SS_TIMEZONE:
        switch (action) {
        case JPP_UI_ACTION_UP:
            if (state->timezone_offset_h < 14) { state->timezone_offset_h++; }
            break;
        case JPP_UI_ACTION_DOWN:
            if (state->timezone_offset_h > -12) { state->timezone_offset_h--; }
            break;
        case JPP_UI_ACTION_OK:
            if (deps->do_ntp_save) {
                deps->do_ntp_save(state->ntp_enabled_staging,
                                  state->ntp_host_staging,
                                  state->timezone_offset_h);
            }
            state->time_ss = JPP_TIME_SS_LIST;
            break;
        case JPP_UI_ACTION_BACK:
            state->time_ss = JPP_TIME_SS_LIST;
            break;
        default: break;
        }
        break;

    default:
        if (action == JPP_UI_ACTION_BACK) { state->time_ss = JPP_TIME_SS_MAIN; }
        break;
    }
    return false;
}

static bool handle_sleep_timers(jpp_settings_state_t *state,
                                const jpp_settings_deps_t *deps,
                                jpp_ui_action_t action)
{
    if (!deps->shell) return action == JPP_UI_ACTION_BACK;
    switch (action) {
    case JPP_UI_ACTION_UP:   if (state->sleep_timers_cursor > 0u) { state->sleep_timers_cursor--; } break;
    case JPP_UI_ACTION_DOWN: if (state->sleep_timers_cursor < 1u) { state->sleep_timers_cursor++; } break;
    case JPP_UI_ACTION_OK:
        if (state->sleep_timers_cursor == 0) {
            int32_t next = next_duration(deps->shell->dim_time_s);
            deps->shell->dim_time_s = next;
            if (deps->do_dim_time_change) deps->do_dim_time_change(next);
        } else {
            int32_t next = next_duration(deps->shell->poweroff_time_s);
            deps->shell->poweroff_time_s = next;
            if (deps->do_poweroff_time_change) deps->do_poweroff_time_change(next);
        }
        break;
    case JPP_UI_ACTION_BACK: return true;
    default: break;
    }
    return false;
}

static bool handle_shutdown_reboot(jpp_settings_state_t *state,
                                    const jpp_settings_deps_t *deps,
                                    jpp_ui_action_t action)
{
    switch (action) {
    case JPP_UI_ACTION_LEFT:
        if (state->shutdown_reboot_cursor > 0u) state->shutdown_reboot_cursor--;
        break;
    case JPP_UI_ACTION_RIGHT:
        if (state->shutdown_reboot_cursor < 1u) state->shutdown_reboot_cursor++;
        break;
    case JPP_UI_ACTION_OK:
        if (state->shutdown_reboot_cursor == 0) {
            if (deps->do_reboot) deps->do_reboot();
        } else {
            if (deps->do_shutdown) deps->do_shutdown();
        }
        break;
    case JPP_UI_ACTION_BACK: return true;
    default: break;
    }
    return false;
}

static bool handle_backup_settings(jpp_settings_state_t *state,
                                    const jpp_settings_deps_t *deps,
                                    jpp_ui_action_t action)
{
    switch (state->backup_ss) {
    case JPP_BACKUP_SS_MAIN:
        switch (action) {
        case JPP_UI_ACTION_UP:
            if (state->backup_cursor > 0u) { state->backup_cursor--; }
            break;
        case JPP_UI_ACTION_DOWN:
            if (state->backup_cursor < 1u) { state->backup_cursor++; }
            break;
        case JPP_UI_ACTION_OK:
            state->backup_result_msg[0] = '\0';
            if (state->backup_cursor == 0u) {
                /* Backup to SD card */
                if (deps->do_settings_backup) {
                    deps->do_settings_backup(state);
                }
                state->backup_ss = JPP_BACKUP_SS_RESULT;
            } else {
                /* Restore from file — may restart device on success. */
                if (deps->do_settings_restore) {
                    deps->do_settings_restore(state);
                }
                /* If we return here the restore was cancelled or failed. */
                if (state->backup_result_msg[0] != '\0') {
                    state->backup_ss = JPP_BACKUP_SS_RESULT;
                }
            }
            break;
        case JPP_UI_ACTION_BACK:
            return true;
        default: break;
        }
        break;

    case JPP_BACKUP_SS_RESULT:
        if (action == JPP_UI_ACTION_OK || action == JPP_UI_ACTION_BACK) {
            state->backup_ss = JPP_BACKUP_SS_MAIN;
        }
        break;

    default:
        if (action == JPP_UI_ACTION_BACK) { return true; }
        break;
    }
    return false;
}

static bool handle_factory_reset(jpp_settings_state_t *state,
                                  const jpp_settings_deps_t *deps,
                                  jpp_ui_action_t action)
{
    switch (action) {
    case JPP_UI_ACTION_UP:   if (state->factory_reset_cursor > 0u) { state->factory_reset_cursor--; } break;
    case JPP_UI_ACTION_DOWN: if (state->factory_reset_cursor < 1u) { state->factory_reset_cursor++; } break;
    case JPP_UI_ACTION_OK:
        if (state->factory_reset_cursor == 1 && deps->do_factory_reset)
            deps->do_factory_reset();
        return true;
    case JPP_UI_ACTION_BACK: return true;
    default: break;
    }
    return false;
}

static bool handle_sound(jpp_settings_state_t *state,
                          const jpp_settings_deps_t *deps,
                          jpp_ui_action_t action)
{
    switch (action) {
    case JPP_UI_ACTION_UP:
        if (state->sound_cursor > 0u) { state->sound_cursor--; }
        break;
    case JPP_UI_ACTION_DOWN:
        if (state->sound_cursor < 2u) { state->sound_cursor++; }
        break;
    case JPP_UI_ACTION_LEFT:
        if (state->sound_cursor == 0u) {
            size_t idx = volume_step_index(state->sound_volume_pct);
            if (idx > 0u) {
                state->sound_volume_pct = VOLUME_STEPS[idx - 1u];
                if (deps->do_volume_change) { deps->do_volume_change(state->sound_volume_pct); }
                jpp_buzzer_play(JPP_BUZZER_SOUND_CLICK);
            }
        } else if (state->sound_cursor == 1u) {
            state->sound_jingle = (state->sound_jingle > 0u)
                ? state->sound_jingle - 1u
                : (uint8_t)(JPP_STARTUP_JINGLE_COUNT - 1u);
            if (deps->do_jingle_change) { deps->do_jingle_change(state->sound_jingle); }
            jpp_buzzer_play_startup_jingle_async((jpp_startup_jingle_t)state->sound_jingle);
        }
        break;
    case JPP_UI_ACTION_RIGHT:
        if (state->sound_cursor == 0u) {
            size_t idx = volume_step_index(state->sound_volume_pct);
            if (idx + 1u < VOLUME_STEP_COUNT) {
                state->sound_volume_pct = VOLUME_STEPS[idx + 1u];
                if (deps->do_volume_change) { deps->do_volume_change(state->sound_volume_pct); }
                jpp_buzzer_play(JPP_BUZZER_SOUND_CLICK);
            }
        } else if (state->sound_cursor == 1u) {
            state->sound_jingle = ((size_t)state->sound_jingle + 1u < JPP_STARTUP_JINGLE_COUNT)
                ? state->sound_jingle + 1u
                : 0u;
            if (deps->do_jingle_change) { deps->do_jingle_change(state->sound_jingle); }
            jpp_buzzer_play_startup_jingle_async((jpp_startup_jingle_t)state->sound_jingle);
        }
        break;
    case JPP_UI_ACTION_OK:
        if (state->sound_cursor == 2u) {
            jpp_buzzer_play_startup_jingle_async((jpp_startup_jingle_t)state->sound_jingle);
        }
        break;
    case JPP_UI_ACTION_BACK:
        return true;
    default: break;
    }
    return false;
}

/* Back action and System apps are LEFT/RIGHT toggles; User's name opens the
   on-screen keyboard on OK.  Unlike the old standalone User's name section,
   editing the name returns to this list rather than closing the section. */
static bool handle_personalisation(jpp_settings_state_t *state,
                                    const jpp_settings_deps_t *deps,
                                    jpp_ui_action_t action)
{
    switch (action) {
    case JPP_UI_ACTION_UP:
        if (state->personalisation_cursor > 0u) { state->personalisation_cursor--; }
        break;
    case JPP_UI_ACTION_DOWN:
        if (state->personalisation_cursor < 2u) { state->personalisation_cursor++; }
        break;
    case JPP_UI_ACTION_LEFT:
    case JPP_UI_ACTION_RIGHT:
        if (state->personalisation_cursor == 0u) {
            state->back_gesture_mode = state->back_gesture_mode ? 0u : 1u;
            if (deps->do_back_gesture_change) {
                deps->do_back_gesture_change(state->back_gesture_mode);
            }
            jpp_buzzer_play(JPP_BUZZER_SOUND_CLICK);
        } else if (state->personalisation_cursor == 2u) {
            state->system_apps_bottom = !state->system_apps_bottom;
            if (deps->do_system_apps_pos_change) {
                deps->do_system_apps_pos_change(state->system_apps_bottom);
            }
            jpp_buzzer_play(JPP_BUZZER_SOUND_CLICK);
        }
        break;
    case JPP_UI_ACTION_OK:
        if (state->personalisation_cursor == 1u) {
            char buf[JPP_SETTINGS_USERNAME_MAX] = {0};
            const char *prefill = state->username_current[0] ? state->username_current : NULL;
            bool ok = deps->do_text_input &&
                      deps->do_text_input("User's name", prefill,
                                          JPP_KBD_TYPE_TEXT, buf, sizeof(buf));
            if (ok && deps->do_username_save) {
                deps->do_username_save(state, buf);
            }
        }
        break;
    case JPP_UI_ACTION_BACK:
        return true;
    default: break;
    }
    return false;
}

static bool handle_device_info(jpp_settings_state_t *state,
                                const jpp_settings_deps_t *deps,
                                jpp_ui_action_t action)
{
    switch (state->lrv_ss) {
    case JPP_LRV_SS_MAIN:
        switch (action) {
        case JPP_UI_ACTION_OK:
            state->lrv_verify_error[0] = '\0';
            if (deps->do_lrv_verify) {
                deps->do_lrv_verify(state);
            }
            if (state->lrv_verify_error[0] != '\0') {
                state->lrv_ss = JPP_LRV_SS_VERIFY_ERROR;
            } else {
                state->lrv_ss = JPP_LRV_SS_VERIFY_RESULT;
            }
            break;
        case JPP_UI_ACTION_BACK:
            if (deps->do_lrv_server_stop) { deps->do_lrv_server_stop(); }
            state->lrv_server_running = false;
            return true;
        default: break;
        }
        break;

    case JPP_LRV_SS_VERIFY_RESULT:
    case JPP_LRV_SS_VERIFY_ERROR:
        /* Any key: stop server and close (or go back to MAIN for error). */
        if (state->lrv_ss == JPP_LRV_SS_VERIFY_ERROR) {
            state->lrv_ss = JPP_LRV_SS_MAIN;
        } else {
            if (deps->do_lrv_server_stop) { deps->do_lrv_server_stop(); }
            state->lrv_server_running = false;
            return true;
        }
        break;

    default: break;
    }
    return false;
}

bool jpp_settings_screen_handle_action(jpp_settings_state_t *state,
                                        const jpp_settings_deps_t *deps,
                                        jpp_ui_action_t action)
{
    if (state == NULL || deps == NULL) return false;

    if (!state->section_open) return handle_top_level(state, deps, action);

    bool close_section = false;
    switch (state->selected_section) {
    case JPP_SETTINGS_SECTION_SHUTDOWN_REBOOT:
        close_section = handle_shutdown_reboot(state, deps, action); break;
    case JPP_SETTINGS_SECTION_PERSONALISATION:
        close_section = handle_personalisation(state, deps, action); break;
    case JPP_SETTINGS_SECTION_WIFI:
        close_section = handle_wifi(state, deps, action); break;
    case JPP_SETTINGS_SECTION_TIME:
        close_section = handle_time(state, deps, action); break;
    case JPP_SETTINGS_SECTION_SLEEP_TIMERS:
        close_section = handle_sleep_timers(state, deps, action); break;
    case JPP_SETTINGS_SECTION_SOUND:
        close_section = handle_sound(state, deps, action); break;
    case JPP_SETTINGS_SECTION_SD_CARD:
        close_section = (action == JPP_UI_ACTION_BACK);  break;
    case JPP_SETTINGS_SECTION_BACKUP:
        close_section = handle_backup_settings(state, deps, action); break;
    case JPP_SETTINGS_SECTION_FACTORY_RESET:
        close_section = handle_factory_reset(state, deps, action); break;
    case JPP_SETTINGS_SECTION_DEVICE_INFO:
        close_section = handle_device_info(state, deps, action); break;
    case JPP_SETTINGS_SECTION_DUMMY_MODE:
        close_section = handle_dummy_mode(state, deps, action); break;
    case JPP_SETTINGS_SECTION_ABOUT:
        close_section = (action == JPP_UI_ACTION_BACK || action == JPP_UI_ACTION_OK); break;
    default:
        close_section = (action == JPP_UI_ACTION_BACK); break;
    }

    if (close_section) state->section_open = false;
    return false;  /* never pop settings from top level here */
}
