#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_random.h"
#include "driver/gpio.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "jpp_boot_core.h"
#include "jpp_settings_core.h"
#include "jpp_sd_core.h"
#include "jpp_oled_core.h"
#include "jpp_rtc_core.h"
#include "jpp_keypad_core.h"
#include "jpp_ui_core.h"
#include "jpp_manifest_core.h"
#include "jpp_battery_core.h"
#include "jpp_fileserver_core.h"
#include "jpp_heap_monitor.h"
#include "jpp_ble_native.h"
#include "jpp_sdk_bridge.h"
#include "jpp_buzzer_core.h"
#include "jpp_draw_util.h"
#include "ssd1306.h"
#include "jpp_hw_config.h"

/* Local modules */
#include "jpp_boot_display.h"
#include "jpp_settings_load.h"
#include "jpp_file_util.h"
#include "jpp_nvs_util.h"
#include "jpp_hw_init.h"
#include "jpp_wifi_init.h"
#include "jpp_native_services.h"
#include "jpp_app_dispatch.h"
#include "jpp_bg_scheduler.h"
#include "jpp_settings_screen.h"
#include "jpp_keyboard.h"
#include "jpp_onboarding.h"
#include "jpp_file_picker.h"
#include "jpp_string_util.h"
#include "jpp_icons.h"
#include "jpp_native_loader_core.h"
#include "jpp_serial_mgr.h"
#include "jpp_backup_restore.h"
#include "jpp_lrv.h"
#include "jpp_lrv_server.h"

static const char *TAG = "jppdos";

/* ---- Constants ---------------------------------------------------------- */

#ifdef JPP_WOKWI_SIM
typedef struct { gpio_num_t gpio; int center_uv; } gpio_sim_btn_t;
static const gpio_sim_btn_t GPIO_SIM_BTNS[] = {
    { JPP_HW_SIM_UP_GPIO,       396498 },
    { JPP_HW_SIM_DOWN_GPIO,     902564 },
    { JPP_HW_SIM_LEFT_GPIO,       3223 },
    { JPP_HW_SIM_RIGHT_GPIO,  1377949 },
    { JPP_HW_SIM_CENTER_GPIO, 1995531 },
};
#define GPIO_SIM_BTN_COUNT (sizeof(GPIO_SIM_BTNS)/sizeof(GPIO_SIM_BTNS[0]))
#endif

static const jpp_keypad_band_t KEYPAD_BANDS[] = {
    { .key = "LEFT",   .center_uv =    3223, .tolerance_uv = 200000, .repeatable = true  },
    { .key = "UP",     .center_uv =  396498, .tolerance_uv = 260000, .repeatable = true  },
    { .key = "DOWN",   .center_uv =  902564, .tolerance_uv = 260000, .repeatable = true  },
    { .key = "RIGHT",  .center_uv = 1377949, .tolerance_uv = 320000, .repeatable = true  },
    { .key = "CENTER", .center_uv = 1995531, .tolerance_uv = 345000, .repeatable = false },
};

/*
 * Minimal valid schema-v2 default settings.
 * settings.json holds only what the firmware reads from it: the schema marker,
 * Wi-Fi credentials (policy.wifi), and the recovery flag (policy.recovery).
 * All other persisted state lives in NVS (jpp_time, jpp_power, jpp_sound,
 * jpp_webdav, jpp_user, jpp_lrv).
 */
static const char DEFAULT_SETTINGS[] =
    "{\"schema_version\":2"
    ",\"policy\":{"
        "\"wifi\":{\"preferred_ssid\":\"\",\"password\":\"\"}"
        ",\"recovery\":{\"force_recovery\":false}"
    "}"
    "}";

// Max. 21 chars
static const char *BIG_DIM_CLOCK_LINES[] = {
    "u r handsome <3",
    "time to shine",
    "hi (non-sexual)",
    "hi",
    "you're late",
    "don't use telega",
    "huh",
    "shit on a lamp post",
    "hamburger",
    "Burmalda",
    "1536 gang",
    "goshan wtf",
    "artyom wtf",
    "peter griffin",
    "xvvzxds",
    "[! BOMB ARMED !]",
    "WELCOME CAROL",
    "Big Dim Clock Line",
    "undefined",
    "(^._.^ )__ cat",
    "j++device, 2026",
    "okak",
    "bazinga",
    "chewsday innit",
    "eto puhosos"
};
static const int BIG_DIM_CLOCK_LINE_COUNT = sizeof(BIG_DIM_CLOCK_LINES) / sizeof(BIG_DIM_CLOCK_LINES[0]);

/* ---- Custom clock lines from /sd/clocklines.txt -------------------------- */

#define CLOCKLINES_PATH      "/sd/clocklines.txt"
#define CLOCKLINES_MAX        64
#define CLOCKLINES_BUF_SIZE 2048

static char   s_clocklines_buf[CLOCKLINES_BUF_SIZE];
static const char *s_clocklines[CLOCKLINES_MAX];
static int    s_clocklines_count  = 0;
static bool   s_clocklines_replace = false;

static void load_clocklines(void)
{
    s_clocklines_count   = 0;
    s_clocklines_replace = false;

    long n = jpp_read_file_into(CLOCKLINES_PATH, s_clocklines_buf,
                                sizeof(s_clocklines_buf));
    if (n <= 0) { return; }

    int   count = 0;
    char *p     = s_clocklines_buf;

    while (*p && count < CLOCKLINES_MAX) {
        while (*p == '\r' || *p == '\n') { p++; }
        if (*p == '\0') { break; }

        char *line = p;
        while (*p && *p != '\r' && *p != '\n') { p++; }
        if (*p) { *p++ = '\0'; }

        if (count == 0 && strcmp(line, "!r") == 0) {
            s_clocklines_replace = true;
            continue;
        }

        if (line[0] == '\0') { continue; }
        s_clocklines[count++] = line;
    }

    s_clocklines_count = count;
    ESP_LOGI("dim_screen", "loaded %d custom clocklines (replace=%d)",
             s_clocklines_count, (int)s_clocklines_replace);
}

/* ---- Keypad task --------------------------------------------------------- */

#define JPP_KEYPAD_POLL_MS      20u
#define JPP_UI_REFRESH_MS      100u
#define JPP_ACTION_QUEUE_DEPTH   8u

QueueHandle_t s_action_queue = NULL;

typedef struct {
    adc_oneshot_unit_handle_t adc;
    SemaphoreHandle_t         adc_mutex;
    jpp_keypad_config_t       cfg;
    jpp_keypad_state_t        state;
} keypad_task_ctx_t;

static void keypad_task(void *arg)
{
    keypad_task_ctx_t *ctx = (keypad_task_ctx_t *)arg;

    while (true) {
        int sample_uv;
#ifdef JPP_WOKWI_SIM
        sample_uv = 2700000;
        for (size_t b = 0u; b < GPIO_SIM_BTN_COUNT; b++) {
            if (gpio_get_level(GPIO_SIM_BTNS[b].gpio) == 0) {
                sample_uv = GPIO_SIM_BTNS[b].center_uv;
                break;
            }
        }
#else
        int raw = 0;
        xSemaphoreTake(ctx->adc_mutex, portMAX_DELAY);
        adc_oneshot_read(ctx->adc, JPP_HW_KEYPAD_ADC_CH, &raw);
        xSemaphoreGive(ctx->adc_mutex);
        sample_uv = (int)((int64_t)raw * JPP_HW_KEYPAD_FULL_SCALE_UV / JPP_HW_ADC_12BIT_MAX_RAW);
#endif

        jpp_keypad_event_t events[8];
        size_t event_count = 0u;
        jpp_keypad_poll(&ctx->state, &ctx->cfg, sample_uv, true,
                        events, 8u, &event_count);

        for (size_t i = 0u; i < event_count; i++) {
            ESP_LOGI(TAG, "KEY_EVENT kind=%s key=%s mapped=%s",
                     jpp_keypad_event_kind_name(events[i].kind),
                     events[i].key    ? events[i].key    : "(null)",
                     events[i].mapped ? events[i].mapped : "(null)");

            jpp_ui_action_t action = jpp_ui_normalize_action(&events[i]);
            if (action != JPP_UI_ACTION_NONE) {
                xQueueSend(s_action_queue, &action, 0);

                if (s_active_sdk_context != NULL) {
                    jpp_sdk_key_event_t sdk_key = JPP_SDK_KEY_NONE;
                    switch (action) {
                    case JPP_UI_ACTION_UP:    sdk_key = JPP_SDK_KEY_UP;     break;
                    case JPP_UI_ACTION_DOWN:  sdk_key = JPP_SDK_KEY_DOWN;   break;
                    case JPP_UI_ACTION_LEFT:  sdk_key = JPP_SDK_KEY_LEFT;   break;
                    case JPP_UI_ACTION_RIGHT: sdk_key = JPP_SDK_KEY_RIGHT;  break;
                    case JPP_UI_ACTION_OK:    sdk_key = JPP_SDK_KEY_CENTER; break;
                    case JPP_UI_ACTION_BACK:  sdk_key = JPP_SDK_KEY_NONE;   break;
                    case JPP_UI_ACTION_NONE:  sdk_key = JPP_SDK_KEY_NONE;   break;
                    }
                    if (events[i].kind == JPP_KEYPAD_KIND_CENTER_LONG) {
                        sdk_key = JPP_SDK_KEY_CENTER_LONG;
                    }
                    if (sdk_key != JPP_SDK_KEY_NONE) {
                        jpp_sdk_push_key(s_active_sdk_context, sdk_key);
                        if (s_sd_task != NULL && s_sd_is_mp &&
                            s_active_sdk_context == &s_sd_ctx) {
                            jpp_vm_request_t act_req;
                            memset(&act_req, 0, sizeof(act_req));
                            act_req.kind = JPP_VM_REQUEST_ACTION;
                            act_req.action_payload = (uint32_t)sdk_key;
                            strncpy(act_req.app_id, s_sd_ctx.app_id,
                                    sizeof(act_req.app_id) - 1u);
                            jpp_vm_schedule_request(&s_sd_vm, "keypad", &act_req);
                        }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(JPP_KEYPAD_POLL_MS));
    }
}


/* ---- Big clock screen (dim state on launcher) --------------------------- */

const char *random_text_line;
static void pick_random_line(void)
{
    int total;
    if (s_clocklines_replace && s_clocklines_count > 0) {
        total = s_clocklines_count;
    } else {
        total = BIG_DIM_CLOCK_LINE_COUNT + s_clocklines_count;
    }
    if (total == 0) { total = BIG_DIM_CLOCK_LINE_COUNT; }

    uint32_t index = esp_random() % (uint32_t)total;

    if (s_clocklines_replace && s_clocklines_count > 0) {
        random_text_line = s_clocklines[index];
    } else if ((int)index < BIG_DIM_CLOCK_LINE_COUNT) {
        random_text_line = BIG_DIM_CLOCK_LINES[index];
    } else {
        random_text_line = s_clocklines[index - BIG_DIM_CLOCK_LINE_COUNT];
    }

    ESP_LOGI("dim_screen", "picked random line i=%lu, val=%s",
             (unsigned long)index, random_text_line);
}

static void render_dim_clock(jpp_rtc_state_t *rtc_state)
{
    jpp_rtc_datetime_t now;
    bool has_time = (rtc_state != NULL) &&
                    (jpp_rtc_get_current(rtc_state, &now) == JPP_RTC_STATUS_OK);
    ssd1306_clear();

    /* Large HH:MM — 2× font, centred on pages 1-2.  With no RTC hardware and no
       NTP sync the time is unknown, so show "--:--" rather than a wrong value. */
    char time_str[6];
    if (has_time) {
        snprintf(time_str, sizeof(time_str), "%02d:%02d", now.hour, now.minute);
    } else {
        snprintf(time_str, sizeof(time_str), "--:--");
    }
    ssd1306_draw_string_2x(1u, (128u - 5u * 12u) / 2u, time_str, false);

    /* Date dd.mm.yyyy on page 4 */
    char date_str[11];
    if (has_time) {
        snprintf(date_str, sizeof(date_str), "%02d.%02d.%04d",
                 now.day, now.month, now.year);
    } else {
        snprintf(date_str, sizeof(date_str), "--.--.----");
    }
    uint8_t date_col = (uint8_t)((128u - strlen(date_str) * 6u) / 2u);
    ssd1306_draw_string(4u, date_col, date_str, false);

    /* Random funny text line */
    uint8_t randline_col = (uint8_t)((128u - strlen(random_text_line) * 6u) / 2u);
    ssd1306_draw_string(7u, randline_col, random_text_line, false);

    ssd1306_flush();
}

/* ---- NTP config (persisted in NVS) -------------------------------------- */

#define JPP_NVS_TIME_NS   "jpp_time"
#define JPP_NVS_POWER_NS  "jpp_power"
#define JPP_NVS_WEBDAV_NS "jpp_webdav"
#define JPP_NVS_SOUND_NS  "jpp_sound"
#define JPP_NVS_USER_NS   "jpp_user"
#define JPP_NVS_DUMMY_NS  "jpp_dummy"

typedef struct {
    bool enabled;
    char host[JPP_SETTINGS_NTP_HOST_MAX];
    int  tz_offset_h;
} ntp_cfg_t;

static ntp_cfg_t s_ntp_cfg = { .host = "time.nist.gov" };
static bool              s_ntp_started    = false;
static volatile bool     s_ntp_time_ready = false;
static jpp_ui_shell_t   *s_main_shell     = NULL;
static jpp_rtc_state_t  *s_rtc_for_backup = NULL;
static jpp_rtc_state_t  *s_rtc_for_lrv    = NULL;

static void ntp_cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open(JPP_NVS_TIME_NS, NVS_READWRITE, &h) != ESP_OK) { return; }
    nvs_set_u8(h, "ntp_en",   (uint8_t)s_ntp_cfg.enabled);
    nvs_set_str(h, "ntp_host", s_ntp_cfg.host);
    nvs_set_i8(h, "tz_h",     (int8_t)s_ntp_cfg.tz_offset_h);
    nvs_commit(h);
    nvs_close(h);
}

static void ntp_cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(JPP_NVS_TIME_NS, NVS_READONLY, &h) != ESP_OK) { return; }
    uint8_t ntp_en = 0;
    if (nvs_get_u8(h, "ntp_en", &ntp_en) == ESP_OK) {
        s_ntp_cfg.enabled = (bool)ntp_en;
    }
    size_t len = sizeof(s_ntp_cfg.host);
    nvs_get_str(h, "ntp_host", s_ntp_cfg.host, &len);
    int8_t tz = 0;
    if (nvs_get_i8(h, "tz_h", &tz) == ESP_OK) { s_ntp_cfg.tz_offset_h = (int)tz; }
    nvs_close(h);
}

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_ntp_time_ready = true;
}

static void ntp_start(void)
{
    const char *host = s_ntp_cfg.host[0] ? s_ntp_cfg.host : "time.nist.gov";
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, host);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_init();
    s_ntp_started    = true;
    s_ntp_time_ready = false;
    ESP_LOGI("ntp", "NTP started, server=%s tz%+d", host, s_ntp_cfg.tz_offset_h);
}

static void ntp_apply(jpp_rtc_state_t *rtc_state)
{
    if (!s_ntp_time_ready) { return; }
    s_ntp_time_ready = false;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t local = tv.tv_sec + (time_t)s_ntp_cfg.tz_offset_h * 3600LL;
    struct tm tm_info;
    gmtime_r(&local, &tm_info);
    jpp_rtc_datetime_t dt = {
        .year       = 1900 + tm_info.tm_year,
        .month      = 1 + tm_info.tm_mon,
        .day        = tm_info.tm_mday,
        .weekday    = tm_info.tm_wday,
        .hour       = tm_info.tm_hour,
        .minute     = tm_info.tm_min,
        .second     = tm_info.tm_sec,
        .subseconds = 0,
    };
    if (!jpp_rtc_datetime_valid(&dt)) {
        ESP_LOGW("ntp", "NTP returned invalid datetime, skipping");
        return;
    }
    jpp_rtc_set_time(rtc_state, &dt);
    if (rtc_state->hw_attached) { jpp_rtc_hw_write(rtc_state, &dt); }
    ESP_LOGI("ntp", "time applied %04d-%02d-%02d %02d:%02d:%02d tz%+d",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
             s_ntp_cfg.tz_offset_h);
}

/* ---- Settings callbacks ------------------------------------------------- */

static void settings_do_dim_time_change(int32_t seconds)
{
    jpp_nvs_set_i32(JPP_NVS_POWER_NS, "dim_s", seconds);
}

static void settings_do_poweroff_time_change(int32_t seconds)
{
    jpp_nvs_set_i32(JPP_NVS_POWER_NS, "poweroff_s", seconds);
}

static void settings_do_ntp_save(bool enabled, const char *host, int tz_offset_h)
{
    s_ntp_cfg.enabled    = enabled;
    s_ntp_cfg.tz_offset_h = tz_offset_h;
    if (host != NULL && host[0] != '\0') {
        strncpy(s_ntp_cfg.host, host, sizeof(s_ntp_cfg.host) - 1u);
        s_ntp_cfg.host[sizeof(s_ntp_cfg.host) - 1u] = '\0';
    }
    ntp_cfg_save();
    if (!enabled && s_ntp_started) {
        esp_sntp_stop();
        s_ntp_started = false;
    }
}

static void settings_do_factory_reset(void)
{
    remove(SETTINGS_PATH);
    remove(SETTINGS_TMP_PATH);
    /* The LRV identity survives factory reset (it lives on the external EEPROM,
       untouched by nvs_flash_erase), but re-lock it so the reset device requires
       the sticker password again. */
    jpp_lrv_relock();
    nvs_flash_erase();
    esp_restart();
}

static void settings_do_reboot(void)
{
    esp_restart();
}

static void settings_do_shutdown(void)
{
    ssd1306_display_on(false);
    gpio_config_t wakeup_io = {
        .pin_bit_mask  = 1ULL << JPP_HW_KEYPAD_GPIO,
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&wakeup_io);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_enable_gpio_wakeup(
        1ULL << JPP_HW_KEYPAD_GPIO,
        ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

/* settings_do_text_input — delegates to the shared jpp_keyboard_input which
   uses jpp_kbd_core for pixel rendering (same implementation as SDK keyboard). */
static bool settings_do_text_input(const char *title, const char *prefill,
                                    jpp_kbd_input_type_t type,
                                    char *out, size_t out_len)
{
    return jpp_keyboard_input(title, prefill, type, s_main_shell, out, out_len);
}

/* ---- Settings backup / restore ------------------------------------------ */

#define BACKUP_DIR  "/sd/backups"

/* Static buffers for backup I/O (serialised by main-loop; no concurrency). */
static char s_backup_file_buf[8192];
static char s_backup_path_buf[256];

static void settings_do_backup(jpp_settings_state_t *state)
{
    /* Build a timestamped filename using the RTC if available. */
    if (s_rtc_for_backup != NULL) {
        jpp_rtc_datetime_t now;
        if (jpp_rtc_get_current(s_rtc_for_backup, &now) == JPP_RTC_STATUS_OK) {
            snprintf(s_backup_path_buf, sizeof(s_backup_path_buf),
                     BACKUP_DIR "/settings_%04d%02d%02d_%02d%02d%02d.json",
                     now.year, now.month, now.day,
                     now.hour, now.minute, now.second);
        } else {
            snprintf(s_backup_path_buf, sizeof(s_backup_path_buf),
                     BACKUP_DIR "/settings_backup.json");
        }
    } else {
        snprintf(s_backup_path_buf, sizeof(s_backup_path_buf),
                 BACKUP_DIR "/settings_backup.json");
    }

    /* Ensure backup directory exists. */
    mkdir(BACKUP_DIR, 0755);

    /* Read settings.json into a cJSON object. */
    long sz = jpp_read_file_into(SETTINGS_PATH, s_backup_file_buf,
                                 sizeof(s_backup_file_buf));
    if (sz == JPP_READ_ERR_OPEN) {
        snprintf(state->backup_result_msg, sizeof(state->backup_result_msg),
                 "Error: no settings file");
        return;
    }
    if (sz < 0) {
        snprintf(state->backup_result_msg, sizeof(state->backup_result_msg),
                 "Error: settings too large");
        return;
    }

    cJSON *settings_obj = cJSON_Parse(s_backup_file_buf);
    if (settings_obj == NULL) {
        snprintf(state->backup_result_msg, sizeof(state->backup_result_msg),
                 "Error: corrupt settings");
        return;
    }

    /* Read NVS namespaces. */
    nvs_handle_t h;
    cJSON *nvs_time = cJSON_CreateObject();
    if (nvs_open(JPP_NVS_TIME_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t ntp_en = 0u;
        nvs_get_u8(h, "ntp_en", &ntp_en);
        char ntp_host[JPP_SETTINGS_NTP_HOST_MAX] = "time.nist.gov";
        size_t hlen = sizeof(ntp_host);
        nvs_get_str(h, "ntp_host", ntp_host, &hlen);
        int8_t tz_h = 0;
        nvs_get_i8(h, "tz_h", &tz_h);
        nvs_close(h);
        cJSON_AddNumberToObject(nvs_time, "ntp_en",   (double)ntp_en);
        cJSON_AddStringToObject(nvs_time, "ntp_host", ntp_host);
        cJSON_AddNumberToObject(nvs_time, "tz_h",     (double)tz_h);
    }

    cJSON *nvs_power = cJSON_CreateObject();
    if (nvs_open(JPP_NVS_POWER_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t dim_s = 60, poweroff_s = 300;
        nvs_get_i32(h, "dim_s",      &dim_s);
        nvs_get_i32(h, "poweroff_s", &poweroff_s);
        nvs_close(h);
        cJSON_AddNumberToObject(nvs_power, "dim_s",      (double)dim_s);
        cJSON_AddNumberToObject(nvs_power, "poweroff_s", (double)poweroff_s);
    }

    cJSON *nvs_webdav = cJSON_CreateObject();
    if (nvs_open(JPP_NVS_WEBDAV_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t pass_static = 0u;
        nvs_get_u8(h, "pass_static", &pass_static);
        char static_pass[JPP_UI_WEBDAV_PASS_MAX + 1u] = "";
        size_t plen = sizeof(static_pass);
        nvs_get_str(h, "static_pass", static_pass, &plen);
        nvs_close(h);
        cJSON_AddNumberToObject(nvs_webdav, "pass_static", (double)pass_static);
        cJSON_AddStringToObject(nvs_webdav, "static_pass", static_pass);
    }

    cJSON *nvs_sound = cJSON_CreateObject();
    if (nvs_open(JPP_NVS_SOUND_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t buzzer_vol = 100u;
        nvs_get_u8(h, "buzzer_vol", &buzzer_vol);
        uint8_t jingle = JPP_STARTUP_JINGLE_DEFAULT;
        nvs_get_u8(h, "startup_jingle", &jingle);
        nvs_close(h);
        cJSON_AddNumberToObject(nvs_sound, "buzzer_vol",     (double)buzzer_vol);
        cJSON_AddNumberToObject(nvs_sound, "startup_jingle", (double)jingle);
    }

    cJSON *nvs_user = cJSON_CreateObject();
    if (nvs_open(JPP_NVS_USER_NS, NVS_READONLY, &h) == ESP_OK) {
        char uname[JPP_SETTINGS_USERNAME_MAX] = "";
        size_t ulen = sizeof(uname);
        nvs_get_str(h, "username", uname, &ulen);
        nvs_close(h);
        cJSON_AddStringToObject(nvs_user, "username", uname);
    }

    /* Assemble the backup JSON. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "jppdos_backup", 1.0);
    cJSON_AddItemToObject(root, "settings",   settings_obj);
    cJSON_AddItemToObject(root, "nvs_time",   nvs_time);
    cJSON_AddItemToObject(root, "nvs_power",  nvs_power);
    cJSON_AddItemToObject(root, "nvs_webdav", nvs_webdav);
    cJSON_AddItemToObject(root, "nvs_sound",  nvs_sound);
    cJSON_AddItemToObject(root, "nvs_user",   nvs_user);

    /* LRV identity is intentionally NOT included in backups: it lives on the
       AT24C32 EEPROM (bound to the RTC module) and is neither user-backupable
       nor restorable — it is provisioned once at manufacturing. */

    char *json_out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_out == NULL) {
        snprintf(state->backup_result_msg, sizeof(state->backup_result_msg),
                 "Error: out of memory");
        return;
    }

    FILE *out = fopen(s_backup_path_buf, "w");
    if (out == NULL) {
        free(json_out);
        snprintf(state->backup_result_msg, sizeof(state->backup_result_msg),
                 "Error: no SD card");
        return;
    }
    fputs(json_out, out);
    fclose(out);
    free(json_out);

    /* Report only the filename (not the full path) so it fits on screen. */
    const char *fname = strrchr(s_backup_path_buf, '/');
    fname = fname ? fname + 1 : s_backup_path_buf;
    snprintf(state->backup_result_msg, sizeof(state->backup_result_msg),
             "Saved:\n%.55s", fname);
    ESP_LOGI(TAG, "BACKUP_OK path=%s", s_backup_path_buf);
}

static void settings_do_restore(jpp_settings_state_t *state)
{
    char path[256] = {0};
    if (!jpp_file_picker("/sd", s_main_shell, path, sizeof(path))) {
        return;  /* user cancelled */
    }

    /* Read backup file. */
    long sz = jpp_read_file_into(path, s_backup_file_buf,
                                 sizeof(s_backup_file_buf));
    if (sz == JPP_READ_ERR_OPEN) {
        snprintf(state->backup_result_msg, sizeof(state->backup_result_msg),
                 "Error: cannot open file");
        return;
    }
    if (sz < 0) {
        snprintf(state->backup_result_msg, sizeof(state->backup_result_msg),
                 "Error: file too large");
        return;
    }

    if (!jpp_backup_apply_json(s_backup_file_buf, state->backup_result_msg,
                               sizeof(state->backup_result_msg))) {
        return;
    }

    ESP_LOGI(TAG, "RESTORE_OK path=%s", path);

    /* Show brief "Restoring..." message before restart. */
    ssd1306_clear();
    ssd1306_draw_string(3, 0, "Restored!", false);
    ssd1306_draw_string(4, 0, "Restarting...", false);
    ssd1306_flush();
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* ---- LRV callbacks ------------------------------------------------------ */

static void settings_do_lrv_unlock(jpp_settings_state_t *state,
                                    const char *password)
{
    jpp_lrv_result_t rc = jpp_lrv_unlock(password);
    if (rc == JPP_LRV_OK) {
        state->lrv_is_unlocked = true;
        state->lrv_unlock_error[0] = '\0';
        jpp_lrv_get_display_info(&state->lrv_serial, state->lrv_pubkey_str);
    } else {
        state->lrv_is_unlocked = false;
        const char *msg = (rc == JPP_LRV_ERR_WRONG_PASSWORD)
                          ? "Wrong password." : "Unlock failed.";
        strncpy(state->lrv_unlock_error, msg, sizeof(state->lrv_unlock_error) - 1u);
        state->lrv_unlock_error[sizeof(state->lrv_unlock_error) - 1u] = '\0';
    }
}

static void settings_do_lrv_verify(jpp_settings_state_t *state)
{
    /* Log LRV verification data at WARN level for serial capture. */
    jpp_lrv_data_t d;
    if (jpp_lrv_get_full_data(&d) != JPP_LRV_OK) {
        strncpy(state->lrv_verify_error, "LRV data unavailable.",
                sizeof(state->lrv_verify_error) - 1u);
        return;
    }

    char challenge[JPP_LRV_CHALLENGE_MAX];
    jpp_lrv_build_challenge(s_rtc_for_lrv, challenge, sizeof(challenge),
                            NULL, 0u, NULL, NULL);

    uint8_t resp_sig[64] = {0};
    jpp_lrv_sign_challenge(challenge, resp_sig);

    /* Format binary fields as hex for logging. */
    char cert_sig_hex[129];
    char pubkey_hex[65];
    char respsig_hex[129];
    jpp_lrv_hex_format(d.cert_sig,      64u, 0u, cert_sig_hex, sizeof(cert_sig_hex));
    jpp_lrv_hex_format(d.device_pubkey, 32u, 0u, pubkey_hex,   sizeof(pubkey_hex));
    jpp_lrv_hex_format(resp_sig,        64u, 0u, respsig_hex,  sizeof(respsig_hex));

    ESP_LOGW(TAG, "LRV cert=%s", d.cert);
    ESP_LOGW(TAG, "LRV cert_sig=%s", cert_sig_hex);
    ESP_LOGW(TAG, "LRV device_pubkey=%s", pubkey_hex);
    ESP_LOGW(TAG, "LRV challenge=%s", challenge);
    ESP_LOGW(TAG, "LRV resp_sig=%s", respsig_hex);

    /* Start HTTP verification server. */
    jpp_lrv_server_result_t srv_rc = jpp_lrv_server_start(s_rtc_for_lrv);
    if (srv_rc == JPP_LRV_SERVER_OK) {
        state->lrv_server_running = true;
        jpp_lrv_server_get_addr(state->lrv_server_addr,
                                 sizeof(state->lrv_server_addr));
        state->lrv_verify_error[0] = '\0';
    } else {
        state->lrv_server_running = false;
        state->lrv_server_addr[0] = '\0';
        if (srv_rc == JPP_LRV_SERVER_ERR_WEBDAV_RUNNING) {
            strncpy(state->lrv_verify_error, "Stop WebDAV server first.",
                    sizeof(state->lrv_verify_error) - 1u);
        } else if (srv_rc == JPP_LRV_SERVER_ERR_NO_WIFI) {
            /* No error: we still logged the cert. Server just not available. */
            state->lrv_verify_error[0] = '\0';
        } else {
            strncpy(state->lrv_verify_error, "Server start failed.",
                    sizeof(state->lrv_verify_error) - 1u);
        }
    }
}

static void settings_do_lrv_server_stop(void)
{
    jpp_lrv_server_stop();
}

/* Blocking connect — called from settings after "Connecting..." screen is shown. */
static void settings_do_wifi_connect(jpp_settings_state_t *state,
                                      const char *ssid, const char *password)
{
    char errmsg[48] = {0};
    bool ok = wifi_connect(ssid, password, 15000u, errmsg, sizeof(errmsg));

    if (ok) {
        snprintf(state->wifi_status_msg, sizeof(state->wifi_status_msg),
                 "Connected: %.18s", ssid);
        jpp_settings_save_wifi(ssid, password);
        strncpy(state->wifi_saved_ssid, ssid, sizeof(state->wifi_saved_ssid) - 1u);
        state->wifi_saved_ssid[sizeof(state->wifi_saved_ssid) - 1u] = '\0';
        ESP_LOGI(TAG, "SETTINGS_WIFI: connected and saved");
    } else {
        snprintf(state->wifi_status_msg, sizeof(state->wifi_status_msg),
                 "Failed: %.38s", errmsg[0] ? errmsg : "unknown error");
        ESP_LOGW(TAG, "SETTINGS_WIFI: connect failed (%s)", errmsg);
    }
}

/* Disconnect — clear saved status and call wifi_disconnect(). */
static void settings_do_wifi_disconnect(jpp_settings_state_t *state)
{
    wifi_disconnect();
    state->wifi_status_msg[0] = '\0';
    ESP_LOGI(TAG, "SETTINGS_WIFI: disconnected");
}

/* Populate wifi_status_msg and wifi_is_connecting from real connection state.
   Called on section open and on every render tick in the Wi-Fi main subscreen. */
static void settings_do_wifi_check_status(jpp_settings_state_t *state)
{
    char ssid[33] = {0};
    if (wifi_get_connected_ssid(ssid, sizeof(ssid))) {
        snprintf(state->wifi_status_msg, sizeof(state->wifi_status_msg),
                 "Connected: %.18s", ssid);
    } else {
        state->wifi_status_msg[0] = '\0';
    }
    /* Reset cursor to 0 when the connecting state first becomes true so the
       "Cancel" item is pre-selected without the user having to navigate. */
    bool now_connecting = wifi_is_connecting();
    if (now_connecting && !state->wifi_is_connecting) {
        state->wifi_list_cursor = 0;
    }
    state->wifi_is_connecting = now_connecting;
    /* Load the SSID persisted in settings.json for the Known Networks display. */
    if (!wifi_get_saved_ssid(state->wifi_saved_ssid, sizeof(state->wifi_saved_ssid))) {
        state->wifi_saved_ssid[0] = '\0';
    }
}

/* Wi-Fi network scan  — called from settings when user selects "Scan networks". */
static void settings_do_wifi_scan(jpp_settings_state_t *state)
{
    state->wifi_scan_count = 0;

    if (!wifi_ensure_started()) {
        ESP_LOGW(TAG, "WIFI_SCAN: Wi-Fi not available");
        return;
    }

    /* Abort any active auto-reconnect loop so the scan is not blocked by
       ESP_ERR_WIFI_STATE.  The user explicitly wants to pick a new network. */
    if (wifi_is_connecting()) {
        ESP_LOGI(TAG, "WIFI_SCAN: aborting reconnect loop before scan");
        wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* blocking */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WIFI_SCAN start failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t num = JPP_SETTINGS_WIFI_SCAN_MAX;
    wifi_ap_record_t aps[JPP_SETTINGS_WIFI_SCAN_MAX];
    err = esp_wifi_scan_get_ap_records(&num, aps);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WIFI_SCAN get_records failed: %s", esp_err_to_name(err));
        return;
    }

    state->wifi_scan_count = (size_t)num;
    for (uint16_t i = 0u; i < num; i++) {
        strncpy(state->wifi_scan_results[i].ssid,
                (const char *)aps[i].ssid,
                JPP_SETTINGS_SSID_MAX - 1u);
        state->wifi_scan_results[i].ssid[JPP_SETTINGS_SSID_MAX - 1u] = '\0';
        state->wifi_scan_results[i].rssi        = aps[i].rssi;
        state->wifi_scan_results[i].has_password =
            (aps[i].authmode != WIFI_AUTH_OPEN);
    }
    ESP_LOGI(TAG, "WIFI_SCAN found %u networks", (unsigned)num);

    /* Bubble the last-saved network to position 0 so it appears first. */
    if (state->wifi_scan_count > 1u) {
        char saved[JPP_SETTINGS_SSID_MAX] = {0};
        if (wifi_get_saved_ssid(saved, sizeof(saved))) {
            for (size_t i = 1u; i < state->wifi_scan_count; i++) {
                if (strcmp(state->wifi_scan_results[i].ssid, saved) == 0) {
                    jpp_settings_wifi_ap_t tmp = state->wifi_scan_results[0];
                    state->wifi_scan_results[0] = state->wifi_scan_results[i];
                    state->wifi_scan_results[i] = tmp;
                    break;
                }
            }
        }
    }
}

/* ---- WebDAV password config persistence --------------------------------- */

static void load_webdav_settings(jpp_ui_shell_t *shell)
{
    shell->webdav_pass_is_static =
        (bool)jpp_nvs_get_u8(JPP_NVS_WEBDAV_NS, "pass_static",
                             (uint8_t)shell->webdav_pass_is_static);
    if (shell->webdav_pass_is_static) {
        jpp_nvs_get_str(JPP_NVS_WEBDAV_NS, "static_pass",
                        shell->webdav_static_pass,
                        sizeof(shell->webdav_static_pass));
    }
    ESP_LOGI(TAG, "WEBDAV: loaded pass_mode=%s",
             shell->webdav_pass_is_static ? "static" : "random");
}

static void save_webdav_settings(const jpp_ui_shell_t *shell)
{
    jpp_nvs_set_u8(JPP_NVS_WEBDAV_NS, "pass_static",
                   (uint8_t)shell->webdav_pass_is_static);
    if (shell->webdav_pass_is_static) {
        jpp_nvs_set_str(JPP_NVS_WEBDAV_NS, "static_pass",
                        shell->webdav_static_pass);
    } else {
        jpp_nvs_erase_key(JPP_NVS_WEBDAV_NS, "static_pass");
    }
    ESP_LOGI(TAG, "WEBDAV: saved pass_mode=%s",
             shell->webdav_pass_is_static ? "static" : "random");
}

/* Load screen standby/sleep times from NVS into the shell state. */
static void load_screen_settings(jpp_ui_shell_t *shell)
{
    shell->dim_time_s      = jpp_nvs_get_i32(JPP_NVS_POWER_NS, "dim_s",
                                             shell->dim_time_s);
    shell->poweroff_time_s = jpp_nvs_get_i32(JPP_NVS_POWER_NS, "poweroff_s",
                                             shell->poweroff_time_s);
    ESP_LOGI(TAG, "SCREEN: standby=%lds sleep=%lds",
             (long)shell->dim_time_s, (long)shell->poweroff_time_s);
}

/* ---- Buzzer volume ------------------------------------------------------ */

static uint8_t s_buzzer_volume_pct = 100u;
static uint8_t s_startup_jingle    = JPP_STARTUP_JINGLE_DEFAULT;

static void load_buzzer_volume(void)
{
    s_buzzer_volume_pct = jpp_nvs_get_u8(JPP_NVS_SOUND_NS, "buzzer_vol",
                                         s_buzzer_volume_pct);
    uint8_t jingle = jpp_nvs_get_u8(JPP_NVS_SOUND_NS, "startup_jingle",
                                    s_startup_jingle);
    if (jingle < JPP_STARTUP_JINGLE_COUNT) {
        s_startup_jingle = jingle;
    }
    jpp_buzzer_set_volume(s_buzzer_volume_pct);
    ESP_LOGI(TAG, "SOUND: buzzer_vol=%u%% jingle=%u", s_buzzer_volume_pct, s_startup_jingle);
}

static void settings_do_volume_change(uint8_t percent)
{
    s_buzzer_volume_pct = percent;
    jpp_buzzer_set_volume(percent);
    jpp_nvs_set_u8(JPP_NVS_SOUND_NS, "buzzer_vol", percent);
    ESP_LOGI(TAG, "SOUND: volume changed to %u%%", percent);
}

static void settings_do_jingle_change(uint8_t jingle)
{
    if (jingle >= JPP_STARTUP_JINGLE_COUNT) { return; }
    s_startup_jingle = jingle;
    jpp_nvs_set_u8(JPP_NVS_SOUND_NS, "startup_jingle", jingle);
    ESP_LOGI(TAG, "SOUND: jingle changed to %u (%s)",
             jingle, jpp_startup_jingle_name((jpp_startup_jingle_t)jingle));
}

/* ---- Dummy mode (single-app lock) --------------------------------------- */

static bool s_dummy_enabled        = false;
static char s_dummy_app_id[JPP_UI_TEXT_LIMIT] = "";
static bool s_boot_center_held     = false;

/*
 * Sample the keypad ADC to detect CENTER held at power-on.
 * A temporary ADC unit is created and deleted; run_main_loop() creates
 * its own unit separately so there is no double-initialisation conflict.
 */
static bool check_center_held_at_boot(void)
{
#ifdef JPP_WOKWI_SIM
    return false;
#else
    adc_oneshot_unit_handle_t boot_adc;
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = JPP_HW_KEYPAD_ADC_UNIT };
    if (adc_oneshot_new_unit(&ucfg, &boot_adc) != ESP_OK) { return false; }
    adc_oneshot_chan_cfg_t ccfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(boot_adc, JPP_HW_KEYPAD_ADC_CH, &ccfg);
    gpio_set_pull_mode((gpio_num_t)JPP_HW_KEYPAD_GPIO, GPIO_PULLUP_ONLY);

    int held = 0;
    for (int i = 0; i < 5; i++) {
        int raw = 0;
        adc_oneshot_read(boot_adc, JPP_HW_KEYPAD_ADC_CH, &raw);
        int uv = (int)((int64_t)raw * JPP_HW_KEYPAD_FULL_SCALE_UV
                       / JPP_HW_ADC_12BIT_MAX_RAW);
        /* CENTER band: center_uv=1995531, tolerance=345000 */
        if (uv >= (1995531 - 345000) && uv <= (1995531 + 345000)) { held++; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    adc_oneshot_del_unit(boot_adc);
    return held >= 3;
#endif
}

static void load_dummy_mode(void)
{
    s_dummy_enabled = (bool)jpp_nvs_get_u8(JPP_NVS_DUMMY_NS, "dummy_en", 0u);
    if (s_dummy_enabled) {
        jpp_nvs_get_str(JPP_NVS_DUMMY_NS, "dummy_app_id",
                        s_dummy_app_id, sizeof(s_dummy_app_id));
        if (s_dummy_app_id[0] == '\0') {
            s_dummy_enabled = false;
        }
    }
    ESP_LOGI(TAG, "DUMMY_MODE: enabled=%d app=%s",
             (int)s_dummy_enabled, s_dummy_app_id);
}

static void settings_do_dummy_mode_save(bool enabled, const char *app_id)
{
    s_dummy_enabled = enabled;
    jpp_nvs_set_u8(JPP_NVS_DUMMY_NS, "dummy_en", (uint8_t)enabled);
    if (enabled && app_id != NULL && app_id[0] != '\0') {
        strncpy(s_dummy_app_id, app_id, sizeof(s_dummy_app_id) - 1u);
        s_dummy_app_id[sizeof(s_dummy_app_id) - 1u] = '\0';
        jpp_nvs_set_str(JPP_NVS_DUMMY_NS, "dummy_app_id", s_dummy_app_id);
    } else {
        s_dummy_app_id[0] = '\0';
        jpp_nvs_erase_key(JPP_NVS_DUMMY_NS, "dummy_app_id");
    }
    ESP_LOGI(TAG, "DUMMY_MODE: saved enabled=%d app=%s",
             (int)enabled, app_id ? app_id : "(none)");
}

/* ---- Username ----------------------------------------------------------- */

static void settings_do_username_save(jpp_settings_state_t *state,
                                       const char *username)
{
    jpp_nvs_set_str(JPP_NVS_USER_NS, "username", username ? username : "");
    if (state != NULL) {
        strncpy(state->username_current, username ? username : "",
                sizeof(state->username_current) - 1u);
        state->username_current[sizeof(state->username_current) - 1u] = '\0';
    }
    ESP_LOGI(TAG, "USERNAME: saved \"%s\"", username ? username : "");
}

static void load_username(jpp_settings_state_t *state)
{
    jpp_nvs_get_str(JPP_NVS_USER_NS, "username",
                    state->username_current, sizeof(state->username_current));
}

/* ---- Main loop ---------------------------------------------------------- */

static void run_main_loop(jpp_ui_shell_t *shell,
                          jpp_rtc_state_t *rtc_state,
                          const jpp_boot_context_t *boot,
                          bool sd_mounted)
{
#ifdef JPP_WOKWI_SIM
    for (size_t b = 0u; b < GPIO_SIM_BTN_COUNT; b++) {
        gpio_config_t gc = {
            .pin_bit_mask = 1ULL << GPIO_SIM_BTNS[b].gpio,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&gc);
    }
#endif

    /* ADC unit */
    adc_oneshot_unit_handle_t adc;
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = JPP_HW_KEYPAD_ADC_UNIT };
    adc_oneshot_new_unit(&ucfg, &adc);
    adc_oneshot_chan_cfg_t ccfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(adc, JPP_HW_KEYPAD_ADC_CH, &ccfg);
#ifndef JPP_WOKWI_SIM
    gpio_set_pull_mode((gpio_num_t)JPP_HW_KEYPAD_GPIO, GPIO_PULLUP_ONLY);
#endif
    adc_oneshot_config_channel(adc, JPP_HW_BATTERY_ADC_CH, &ccfg);

    jpp_battery_config_t bat_cfg;
    jpp_battery_config_defaults(&bat_cfg);
    jpp_battery_state_t bat_state = { .percent = 0, .valid = false };

    /* Background task scheduler: load the persisted schedule table. */
    jpp_bg_scheduler_init();
    int64_t bg_run_start_us = 0;
    /* Expose the live battery state to the SDK device.status broker callback.
       run_main_loop never returns, so the address stays valid for app sessions. */
    jpp_native_services_set_battery_state(&bat_state);

    /* Keypad task */
    static keypad_task_ctx_t kpad_ctx;
    kpad_ctx.adc       = adc;
    kpad_ctx.adc_mutex = xSemaphoreCreateMutex();
    kpad_ctx.cfg = (jpp_keypad_config_t){
        .enabled            = true,
        .calibration_uv     = 0,
        .hysteresis_uv      = JPP_KEYPAD_DEFAULT_HYSTERESIS_UV,
        .debounce_samples   = JPP_KEYPAD_DEFAULT_DEBOUNCE_SAMPLES,
        .long_press_ms      = JPP_KEYPAD_DEFAULT_LONG_PRESS_MS,
        .poll_interval_ms   = JPP_KEYPAD_POLL_MS,
        .repeat_enabled     = true,
        .repeat_delay_ms    = JPP_KEYPAD_DEFAULT_REPEAT_DELAY_MS,
        .repeat_interval_ms = JPP_KEYPAD_DEFAULT_REPEAT_INTERVAL_MS,
        .bands              = KEYPAD_BANDS,
        .band_count         = sizeof(KEYPAD_BANDS) / sizeof(KEYPAD_BANDS[0]),
    };
    jpp_keypad_state_init(&kpad_ctx.state, &kpad_ctx.cfg);

    s_action_queue = xQueueCreate(JPP_ACTION_QUEUE_DEPTH, sizeof(jpp_ui_action_t));
    xTaskCreate(keypad_task, "keypad", 4096, &kpad_ctx,
                configMAX_PRIORITIES - 1, NULL);

    /* Settings screen state */
    jpp_settings_state_t settings_state;
    sdmmc_card_t *sd_card_handle = jpp_hw_init_sd_card();
    sdmmc_card_t **sd_card_ptr = &sd_card_handle;

    jpp_settings_deps_t settings_deps = {
        .shell                  = shell,
        .rtc                    = rtc_state,
        .sd_card_ptr            = sd_card_ptr,
        .do_wifi_scan           = settings_do_wifi_scan,
        .do_wifi_connect        = settings_do_wifi_connect,
        .do_wifi_disconnect     = settings_do_wifi_disconnect,
        .do_wifi_check_status   = settings_do_wifi_check_status,
        .do_factory_reset       = settings_do_factory_reset,
        .do_ntp_save            = settings_do_ntp_save,
        .do_dim_time_change     = settings_do_dim_time_change,
        .do_poweroff_time_change = settings_do_poweroff_time_change,
        .do_reboot              = settings_do_reboot,
        .do_shutdown            = settings_do_shutdown,
        .do_text_input          = settings_do_text_input,
        .do_volume_change       = settings_do_volume_change,
        .do_jingle_change       = settings_do_jingle_change,
        .do_settings_backup     = settings_do_backup,
        .do_settings_restore    = settings_do_restore,
        .do_lrv_unlock          = settings_do_lrv_unlock,
        .do_lrv_verify          = settings_do_lrv_verify,
        .do_lrv_server_stop     = settings_do_lrv_server_stop,
        .do_username_save       = settings_do_username_save,
        .do_dummy_mode_save     = settings_do_dummy_mode_save,
    };
    s_main_shell     = shell;
    s_rtc_for_backup = rtc_state;
    s_rtc_for_lrv    = rtc_state;
    jpp_settings_screen_init(&settings_state, &settings_deps);

    /* Initialise LRV display state from NVS. */
    settings_state.lrv_has_data    = jpp_lrv_has_data();
    settings_state.lrv_is_unlocked = jpp_lrv_is_unlocked();
    if (settings_state.lrv_is_unlocked) {
        jpp_lrv_get_display_info(&settings_state.lrv_serial,
                                  settings_state.lrv_pubkey_str);
    }

    /* Load persisted username. */
    load_username(&settings_state);

    /* First-boot welcome flow (no-op on every later boot). Runs after the
       keypad task/action queue and LRV/username state above are ready, before
       the launcher UI takes over. */
    jpp_onboarding_run(shell, &settings_state);

    /* Populate dummy mode state from NVS (loaded early in app_main). */
    settings_state.dummy_enabled = s_dummy_enabled;
    if (s_dummy_enabled && s_dummy_app_id[0] != '\0') {
        strncpy(settings_state.dummy_app_id, s_dummy_app_id,
                sizeof(settings_state.dummy_app_id) - 1u);
        settings_state.dummy_app_id[sizeof(settings_state.dummy_app_id) - 1u] = '\0';
        /* Look up app display name from shell catalogue. */
        for (size_t i = 0; i < shell->app_count; i++) {
            if (strcmp(shell->apps[i].app_id, s_dummy_app_id) == 0) {
                strncpy(settings_state.dummy_app_name, shell->apps[i].name,
                        sizeof(settings_state.dummy_app_name) - 1u);
                settings_state.dummy_app_name[sizeof(settings_state.dummy_app_name)-1u] = '\0';
                break;
            }
        }
    }

    /* Load persisted screen timings. */
    load_screen_settings(shell);
    load_webdav_settings(shell);

    /* Populate Sound section staging from values already loaded at boot. */
    settings_state.sound_volume_pct = s_buzzer_volume_pct;
    settings_state.sound_jingle     = s_startup_jingle;

    /* Load persisted NTP / timezone config and populate settings staging state. */
    ntp_cfg_load();
    settings_state.ntp_enabled_staging = s_ntp_cfg.enabled;
    if (s_ntp_cfg.host[0] != '\0') {
        strncpy(settings_state.ntp_host_staging, s_ntp_cfg.host,
                sizeof(settings_state.ntp_host_staging) - 1u);
        settings_state.ntp_host_staging[sizeof(settings_state.ntp_host_staging)-1u] = '\0';
    }
    settings_state.timezone_offset_h = s_ntp_cfg.tz_offset_h;

    /* UI / display loop */
    size_t ui_tick = 0u;
    TickType_t last_wake = xTaskGetTickCount();
    int  last_drawn_bat_pct  = -2;    /* -2 = never drawn; force first draw */
    bool last_drawn_wifi     = false;
    int  last_logged_bat_pct = -2;    /* -2 = never logged; log only on change */
    char prev_top_screen[JPP_UI_TEXT_LIMIT] = "";
    /* Track power state from the PREVIOUS tick to detect transitions correctly.
       note_activity() sets power_state=ACTIVE before tick_power() runs, so we
       must compare against last tick's state — not prev_power captured in the
       same tick — to reliably detect DIM→ACTIVE wake-up transitions. */
    jpp_ui_power_state_t power_state_last_tick = JPP_UI_POWER_ACTIVE;

    /* Track whether any HTTP server (WebDAV or LRV) is running across ticks so we
       suspend the BLE controller (freeing its heap for WiFi) while one is up, and
       resume it once all are stopped.  Both servers and SD apps are mutually
       exclusive, so no app is using BLE while a server runs.  WebDAV and the LRV
       server are themselves mutually exclusive, so "any running" is the right
       gate.  See jpp_ble_native_suspend(). */
    bool server_running_last = false;

    /* Initial RTC read.  With no DS1307 fitted (hw_attached == false) or a failed
       read the time is unknown, so seed the status clock with "--:--"; the
       per-second tick below keeps it in sync (and picks up an NTP sync later). */
    {
        char time_buf[JPP_UI_STATUS_TIME_LEN];
        if (rtc_state != NULL && rtc_state->hw_attached &&
            jpp_rtc_hw_read(rtc_state) == JPP_RTC_STATUS_OK) {
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d",
                     rtc_state->datetime.hour, rtc_state->datetime.minute);
        } else {
            snprintf(time_buf, sizeof(time_buf), "--:--");
        }
        jpp_ui_shell_set_status(shell, time_buf, shell->status_battery_pct);
    }

    while (true) {
        /* Drain action queue */
        jpp_ui_action_t action;
        while (xQueueReceive(s_action_queue, &action, 0) == pdTRUE) {
            /* Any keypress resets inactivity and restores screen */
            jpp_ui_shell_note_activity(shell);

            if (shell->sd_ejected) {
                if (action == JPP_UI_ACTION_OK) { esp_restart(); }
                continue;
            }

            if (s_sd_task != NULL) {
                continue;
            }

            /* Serial manager owns the display during consent / active session. */
            if (jpp_serial_mgr_needs_render()) {
                jpp_serial_mgr_handle_action(action);
                continue;
            }

            const char *screen = jpp_ui_stack_top(&shell->stack);
            if (screen != NULL && strcmp(screen, "settings") == 0) {
                bool pop = jpp_settings_screen_handle_action(
                                &settings_state, &settings_deps, action);
                if (pop) {
                    jpp_ui_stack_pop(&shell->stack);
                    shell->display.has_last_frame = false;
                }
            } else {
                /* Swallow the waking keypress — note_activity() already
                   restored the screen; don't also act on it. */
                if (power_state_last_tick != JPP_UI_POWER_ACTIVE) {
                    ESP_LOGI(TAG, "UI_ACTION %s (ignored on wake)", jpp_ui_action_name(action));
                    continue;
                }
                /* Dummy mode: block all launcher navigation so the user
                   cannot exit to the launcher or open other apps. */
                if (s_dummy_enabled) {
                    continue;
                }
                ESP_LOGI(TAG, "UI_ACTION %s", jpp_ui_action_name(action));
                jpp_ui_shell_handle_action(shell, action);
            }
        }

        /* WebDAV static password input: prompted when user selects "Static password" */
        if (shell->webdav_needs_pass_input) {
            shell->webdav_needs_pass_input = false;
            char new_pass[JPP_UI_WEBDAV_PASS_MAX + 1u] = {0};
            bool got = jpp_keyboard_input("Static password",
                                          shell->webdav_static_pass,
                                          JPP_KBD_TYPE_TEXT,
                                          shell, new_pass, sizeof(new_pass));
            if (got && new_pass[0] != '\0') {
                jpp_str_copy(shell->webdav_static_pass,
                             sizeof(shell->webdav_static_pass), new_pass);
                shell->webdav_pass_is_static      = true;
                shell->webdav_pass_config_changed = true;
                if (shell->fileserver_running) {
                    jpp_fileserver_stop();
                    jpp_fileserver_start_with_password(shell->webdav_static_pass);
                }
            }
            shell->display.has_last_frame = false;
        }

        /* Persist WebDAV password config to NVS when it changes */
        if (shell->webdav_pass_config_changed) {
            shell->webdav_pass_config_changed = false;
            save_webdav_settings(shell);
        }

        /* Battery read every 5 seconds */
        if (ui_tick % (5000u / JPP_UI_REFRESH_MS) == 0u) {
            xSemaphoreTake(kpad_ctx.adc_mutex, portMAX_DELAY);
            jpp_battery_read(adc, &bat_cfg, &bat_state);
            xSemaphoreGive(kpad_ctx.adc_mutex);
            int pct = bat_state.valid ? bat_state.percent : -1;
            /* Log only when the percentage changes — the 5 s poll otherwise
               floods the console with the same value. */
            if (pct != last_logged_bat_pct) {
                ESP_LOGI(TAG, "BATTERY pct=%d valid=%d", pct, (int)bat_state.valid);
                last_logged_bat_pct = pct;
            }
            if (bat_state.valid) {
                jpp_ui_shell_set_status(shell, shell->status_time, pct);
            }
        }

        /* File-server state and Wi-Fi state sync every 2 seconds */
        if (ui_tick % (2000u / JPP_UI_REFRESH_MS) == 0u) {
            jpp_fileserver_status_t fs_status;
            jpp_fileserver_get_status(&fs_status);
            jpp_ui_shell_set_fileserver_state(
                shell,
                fs_status.state == JPP_FILESERVER_STATE_RUNNING,
                fs_status.ip[0] != '\0' ? fs_status.ip : NULL,
                fs_status.port,
                fs_status.password[0] != '\0' ? fs_status.password : NULL
            );
            shell->status_wifi_connected = wifi_is_connected();

            /* When any HTTP server (WebDAV or LRV) starts, free the BLE
               controller's heap so the WiFi driver can allocate management/data
               frames while serving; restore it once all servers stop.  No app
               runs while a server is up, so suspending BLE here is safe. */
            bool server_running_now =
                (fs_status.state == JPP_FILESERVER_STATE_RUNNING) ||
                jpp_lrv_server_is_running();
            if (server_running_now != server_running_last) {
                if (server_running_now) {
                    jpp_ble_native_suspend();
                } else {
                    jpp_ble_native_resume();
                }
                server_running_last = server_running_now;
            }
        }

        /* Live time update + NTP every second */
        if (ui_tick % (1000u / JPP_UI_REFRESH_MS) == 0u && rtc_state != NULL) {
            jpp_rtc_datetime_t now;
            char time_buf[JPP_UI_STATUS_TIME_LEN];
            if (jpp_rtc_get_current(rtc_state, &now) == JPP_RTC_STATUS_OK) {
                snprintf(time_buf, sizeof(time_buf), "%02d:%02d",
                         now.hour, now.minute);
            } else {
                /* No RTC hardware and no NTP sync yet: show a blank clock. */
                snprintf(time_buf, sizeof(time_buf), "--:--");
            }
            jpp_ui_shell_set_status(shell, time_buf, shell->status_battery_pct);
            /* Re-read hardware RTC every 60 seconds to prevent drift */
            if (ui_tick % (60000u / JPP_UI_REFRESH_MS) == 0u &&
                rtc_state->hw_attached) {
                jpp_rtc_hw_read(rtc_state);
            }
            /* NTP: start SNTP once Wi-Fi connects (if enabled and not yet started). */
            if (s_ntp_cfg.enabled && shell->status_wifi_connected && !s_ntp_started) {
                ntp_start();
            }
            /* Apply synced time to RTC when SNTP callback fires. */
            ntp_apply(rtc_state);
        }

        ui_tick++;

        /* Power management tick.
           IMPORTANT: note_activity() (called in the action loop above) already
           set shell->power_state = ACTIVE before tick_power() runs here.
           Comparing against power_state_last_tick (from the *previous* loop
           iteration) is the only reliable way to detect the DIM→ACTIVE wakeup
           transition and restore display brightness. */
        jpp_ui_shell_tick_power(shell);
        {
            jpp_ui_power_state_t now = shell->power_state;

            if (now != power_state_last_tick) {
                if (now == JPP_UI_POWER_DIM) {
                    ssd1306_set_contrast(0x80);
                    ssd1306_set_power_registers(0x00, 0x00); /* max dim */
                    pick_random_line();
                    ESP_LOGI(TAG, "SCREEN_DIM");
                } else if (now == JPP_UI_POWER_ACTIVE &&
                           power_state_last_tick != JPP_UI_POWER_ACTIVE) {
                    ssd1306_set_contrast(0xCF);
                    ssd1306_set_power_registers(0xF1, 0x40); /* restore normal */
                    ssd1306_display_on(true);
                    shell->display.has_last_frame = false;
                    last_drawn_bat_pct = -2;
                    last_drawn_wifi = false;
                    ESP_LOGI(TAG, "SCREEN_WAKE");
                } else if (now == JPP_UI_POWER_OFF) {
                    const char *top_at_sleep = jpp_ui_stack_top(&shell->stack);
                    bool wakelock = (shell->fileserver_running &&
                                     top_at_sleep != NULL &&
                                     strcmp(top_at_sleep, "webdav") == 0) ||
                                    (s_active_sdk_context != NULL &&
                                     s_active_sdk_context->wakelock_held) ||
                                    jpp_serial_mgr_needs_render();
                    if (!wakelock) {
                        if (s_sd_task != NULL) { teardown_sd_app(shell); }
                        ssd1306_display_on(false);
                        gpio_config_t off_wakeup_io = {
                            .pin_bit_mask  = 1ULL << JPP_HW_KEYPAD_GPIO,
                            .mode          = GPIO_MODE_INPUT,
                            .pull_up_en    = GPIO_PULLUP_ENABLE,
                            .pull_down_en  = GPIO_PULLDOWN_DISABLE,
                            .intr_type     = GPIO_INTR_DISABLE,
                        };
                        gpio_config(&off_wakeup_io);
                        vTaskDelay(pdMS_TO_TICKS(100));
                        ESP_LOGI(TAG, "DEEP_SLEEP");
                        esp_deep_sleep_enable_gpio_wakeup(
                            1ULL << JPP_HW_KEYPAD_GPIO,
                            ESP_GPIO_WAKEUP_GPIO_LOW);
                        esp_deep_sleep_start(); /* never returns */
                    } else {
                        jpp_ui_shell_note_activity(shell);
                    }
                }
            }

            power_state_last_tick = now;
        }

        /* Periodic SD probe every 2 seconds (detect card removal).
           Only treat as ejection for hardware/media errors (EIO, ENODEV, ESTALE);
           ENOMEM is transient (BLE+WiFi heap peak) and not logged to avoid noise. */
        if (ui_tick % (2000u / JPP_UI_REFRESH_MS) == 0u &&
            !shell->sd_ejected && sd_mounted) {
            DIR *probe = opendir("/sd");
            if (probe == NULL) {
                if (errno == EIO || errno == ENODEV || errno == ESTALE) {
                    ESP_LOGW(TAG, "SD_PROBE_FAIL errno=%d", errno);
                    s_sd_ejection_detected = true;
                }
                /* ENOMEM and other transient errors: skip silently. */
            } else {
                closedir(probe);
            }
        }

        /* SD ejection check */
        if (!shell->sd_ejected && s_sd_ejection_detected) {
            shell->sd_ejected = true;
            /* Force display back on and to full brightness before showing error */
            ssd1306_display_on(true);
            ssd1306_set_contrast(0xCF);
            jpp_ui_stack_reset_to_root(&shell->stack);
            jpp_ui_stack_push(&shell->stack, "sd_ejected");
            shell->display.has_last_frame = false;
            ESP_LOGW(TAG, "SD_EJECTED fatal screen");
        }

        /* App screen detection: every screen name that is not a built-in
           (reserved) id is an SD app id pushed by the launcher. */
        const char *top_screen = jpp_ui_stack_top(&shell->stack);
        bool sd_app_open = (top_screen != NULL) &&
                           !jpp_manifest_v2_is_reserved_app_id(top_screen);

        /* User preemption: an interactive launch takes priority over a
           running headless background task. */
        if (sd_app_open && jpp_app_bg_running()) {
            ESP_LOGW(TAG, "BG_TASK_PREEMPTED (user launch)");
            jpp_app_bg_teardown();
        }

        if (sd_app_open && s_sd_task == NULL) {
            if (jpp_serial_mgr_session_active()) {
                /* Serial manager holds exclusive SD access; refuse app launch. */
                jpp_ui_stack_pop(&shell->stack);
                sd_app_open = false;
            } else {
                bool launched;
#ifdef JPP_WOKWI_EMBED_APP
                launched = (strcmp(top_screen, JPP_WOKWI_EMBED_APP_ID) == 0)
                    ? launch_wokwi_embed_app(top_screen, boot)
                    : launch_sd_app(top_screen, boot);
#else
                launched = launch_sd_app(top_screen, boot);
#endif
                if (!launched) {
                    jpp_ui_stack_pop(&shell->stack);
                    sd_app_open = false;
                    /* Show the failure reason (the dispatcher already recorded it
                       via record_app_crash, same plumbing as a runtime crash). */
                    char fail_app[32];
                    char fail_reason[32];
                    if (jpp_app_crash_take(fail_app, sizeof(fail_app),
                                           fail_reason, sizeof(fail_reason))) {
                        jpp_ui_shell_record_crash(shell, "LAUNCH_FAILED", fail_app, fail_reason);
                    }
                } else if (s_dummy_enabled) {
                    /* Tag the SDK context so the app can query dummy mode.
                       Set before the app task runs (task starts at low priority,
                       main task continues until vTaskDelayUntil at loop end). */
                    s_sd_ctx.dummy_mode = true;
                }
            }
        }

        /* Headless background-run supervision. */
        if (jpp_app_bg_running()) {
            if (jpp_app_bg_finished()) {
                jpp_app_bg_teardown();
            } else if ((esp_timer_get_time() - bg_run_start_us) / 1000 >
                       (int64_t)JPP_RESOURCE_BG_TASK_RUN_QUOTA_MS) {
                /* Kill on overrun. The task may hold broker locks or the app
                   pool mid-run, so a restart is the only state-safe recovery;
                   last_run was persisted at launch, so the task will not
                   re-fire immediately after boot. */
                ESP_LOGE(TAG, "BG_TASK_KILLED (quota exceeded) — restarting");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        } else if (!sd_app_open && s_sd_task == NULL &&
                   top_screen != NULL && strcmp(top_screen, "launcher") == 0 &&
                   !jpp_serial_mgr_needs_render() &&
                   !shell->fileserver_running &&
                   !jpp_lrv_server_is_running()) {
            /* Idle on the launcher: run a due background task, if any. */
            char bg_app[32];
            char bg_task[32];
            if (jpp_bg_scheduler_due(rtc_state, bg_app, sizeof(bg_app),
                                     bg_task, sizeof(bg_task))) {
                /* Mark before launching so a crash/kill cannot re-fire it. */
                jpp_bg_scheduler_mark_run(rtc_state, bg_app, bg_task);
                if (jpp_app_bg_launch(bg_app, bg_task, boot)) {
                    bg_run_start_us = esp_timer_get_time();
                }
            }
        }

        if (s_sd_task != NULL && !jpp_app_bg_running() &&
            s_sd_ctx.close_requested) {
            teardown_sd_app(shell);
            sd_app_open = false;
            shell->display.has_last_frame = false;
            top_screen = jpp_ui_stack_top(&shell->stack);

            /* If the app ended in a failure, show the crash dialog (the
               dispatcher already emitted APP_CRASH and wrote ui_crash.log). */
            char crash_app[32];
            char crash_reason[32];
            bool app_crashed = jpp_app_crash_take(crash_app, sizeof(crash_app),
                                                   crash_reason, sizeof(crash_reason));
            if (app_crashed) {
                jpp_ui_shell_record_crash(shell, "APP_CRASH", crash_app, crash_reason);
                top_screen = jpp_ui_stack_top(&shell->stack);
            }

            /* Dummy mode: re-push the locked app immediately after teardown
               so it re-launches on the next tick. Skip on crash so the crash
               dialog is shown; the device must be rebooted (with CENTER held
               to escape) to disable dummy mode anyway. */
            if (!app_crashed && s_dummy_enabled && s_dummy_app_id[0] != '\0') {
                jpp_ui_stack_push(&shell->stack, s_dummy_app_id);
                top_screen = jpp_ui_stack_top(&shell->stack);
            }
        }

        /* Background app re-discovery: trigger when returning to launcher. */
        if (ui_tick > 0u &&
            strcmp(prev_top_screen, "launcher") != 0 &&
            top_screen != NULL && strcmp(top_screen, "launcher") == 0) {
            discover_apps_background_start(boot->boot_mode == JPP_BOOT_MODE_NORMAL);
            load_clocklines();
        }
        if (discover_apps_background_ready()) {
            discover_apps_apply_to_shell(shell);
            shell->display.has_last_frame = false;
        }
        strncpy(prev_top_screen,
                top_screen != NULL ? top_screen : "",
                sizeof(prev_top_screen) - 1u);
        prev_top_screen[sizeof(prev_top_screen) - 1u] = '\0';

        /* Dispatch IDLE tick to running MicroPython apps every loop iteration
           so on_idle() fires at approximately JPP_UI_REFRESH_MS cadence. */
        if (sd_app_open && s_sd_task != NULL && s_sd_is_mp) {
            jpp_vm_request_t idle_req;
            memset(&idle_req, 0, sizeof(idle_req));
            idle_req.kind = JPP_VM_REQUEST_IDLE;
            strncpy(idle_req.app_id, s_sd_ctx.app_id,
                    sizeof(idle_req.app_id) - 1u);
            jpp_vm_schedule_request(&s_sd_vm, "main", &idle_req);
        }

        /* ---- Render ---------------------------------------------------- */

        /* Serial manager consent dialog / active-session screen.  Checked
           before every other screen (including Settings) because the action
           loop above already routes all keypad input to the serial manager
           the moment jpp_serial_mgr_needs_render() is true — if this render
           check ran after the Settings screen check, a SESSION_START arriving
           while the user is in Settings would silently steal every keypress
           (consent Allow/Deny) while the OLED kept showing the Settings menu,
           making the device appear frozen. When the serial screen goes away
           (session closed by host, BACK, or timeout) force a launcher redraw
           — otherwise the last serial frame stays on the OLED because the
           shell's frame cache still matches. */
        {
            static bool serial_rendered_last = false;
            bool serial_now = jpp_serial_mgr_needs_render();
            if (serial_rendered_last && !serial_now) {
                shell->display.has_last_frame = false;
            }
            serial_rendered_last = serial_now;
            if (serial_now) {
                jpp_serial_mgr_render();
                vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(JPP_UI_REFRESH_MS));
                continue;
            }
        }

        /* Settings screen: rendered directly */
        if (top_screen != NULL && strcmp(top_screen, "settings") == 0) {
            jpp_settings_screen_render(&settings_state, &settings_deps);
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(JPP_UI_REFRESH_MS));
            continue;
        }

        /* SD ejection fatal screen: render directly with flashing warning */
        if (shell->sd_ejected) {
            ssd1306_clear();
            /* Flashing exclamation — alternate every 500ms (5 ticks) */
            if ((ui_tick / 5u) % 2u == 0u) {
                ssd1306_draw_string(1, 40, "/\\", false);
                ssd1306_draw_string(2, 34, "/!!", false);
                ssd1306_draw_string(3, 28, "/----\\", false);
            }
            ssd1306_draw_string(5, 0, "SD card was ejected.", false);
            ssd1306_draw_string(6, 0, "Insert SD + OK", false);
            ssd1306_draw_string(7, 0, "to restart JPPDOS", false);
            ssd1306_flush();
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(JPP_UI_REFRESH_MS));
            continue;
        }

        /* Dim state: show big clock on launcher (suppressed when WebDAV server is active) */
        if (shell->power_state == JPP_UI_POWER_DIM && !sd_app_open &&
            !shell->fileserver_running) {
            render_dim_clock(rtc_state);
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(JPP_UI_REFRESH_MS));
            continue;
        }

        const jpp_sdk_context_t *active_ctx =
            (sd_app_open && s_sd_task != NULL) ? &s_sd_ctx : NULL;

        if (active_ctx != NULL) {
            ssd1306_clear();
            /* Fullscreen canvas owns all 8 pages; windowed mode draws the
               frame text rows + title rule with the canvas on pages 2–7. */
            size_t  canvas_pages = active_ctx->canvas_fullscreen ? 8u : 6u;
            uint8_t page_base    = active_ctx->canvas_fullscreen ? 0u : 2u;
            if (!active_ctx->canvas_fullscreen) {
                for (size_t row = 0u; row < active_ctx->frame_line_count &&
                                      row < JPP_SDK_FRAME_LINE_CAPACITY; row++) {
                    ssd1306_draw_string((uint8_t)row, 0,
                                        active_ctx->frame_lines[row], false);
                }
                /* Signature line under the title — matches the launcher/settings/
                   WebDAV header style (title on row 0, 1-px rule on page 1). */
                if (active_ctx->frame_title_rule) {
                    jpp_draw_rule(1u);
                }
            }
            for (size_t p = 0u; p < canvas_pages; p++) {
                for (size_t c = 0u; c < 16u; c++) {
                    uint8_t block[8];
                    bool has_pixels = false;
                    for (size_t r = 0u; r < 8u; r++) {
                        block[r] = active_ctx->canvas[p * 8u + r][c];
                        if (block[r]) has_pixels = true;
                    }
                    if (has_pixels) {
                        ssd1306_draw_bitmap_8x8(
                            (uint8_t)(p + page_base), (uint8_t)(c * 8u), block, false);
                    }
                }
            }
            ssd1306_flush();
        } else {
            jpp_ui_frame_t frame;
            bool frame_changed = false;
            jpp_ui_shell_render(shell, &frame, &frame_changed);

            bool on_launcher = (top_screen != NULL &&
                                strcmp(top_screen, "launcher") == 0);
            bool on_webdav = (top_screen != NULL &&
                              strcmp(top_screen, "webdav") == 0);
            bool on_webdav_passconfig = (top_screen != NULL &&
                                         strcmp(top_screen, "webdav_passconfig") == 0);
            int  cur_bat_pct  = bat_state.valid ? bat_state.percent : -1;
            bool bat_changed  = on_launcher && (cur_bat_pct != last_drawn_bat_pct);
            /* Wi-Fi icon: solid when connected, blinks at 5 Hz when connecting. */
            bool blink_on    = ui_tick % 2u == 0u;
            bool wifi_visible = shell->status_wifi_connected ||
                                (wifi_is_connecting() && blink_on);
            bool wifi_changed = on_launcher && (wifi_visible != last_drawn_wifi);

            if (frame_changed || bat_changed || wifi_changed) {
                if (frame_changed) {
                    ESP_LOGI(TAG, "+---------------------+");
                    for (int row = 0; row < (int)JPP_UI_FRAME_LINES; row++) {
                        ESP_LOGI(TAG, "|%-21s|", frame.lines[row]);
                    }
                    ESP_LOGI(TAG, "+---------------------+");

                    ssd1306_clear();
                    for (int row = 0; row < (int)JPP_UI_FRAME_LINES; row++) {
                        ssd1306_draw_string((uint8_t)row, 0,
                                            frame.lines[row], false);
                    }
                    if (on_launcher || on_webdav) {
                        jpp_draw_rule(1u);
                    }
                    /* Checkmark on the active password mode line in passconfig. */
                    if (on_webdav_passconfig) {
                        uint8_t chk_page = shell->webdav_pass_is_static ? 3u : 2u;
                        jpp_icon_draw(chk_page, 120u, JPP_ICON_CHECKMARK);
                    }
                }

                /* Status bar icons: only drawn on the launcher (where the status
                   bar lives). Keeps them off WebDAV, dialog, crash screens. */
                if (on_launcher) {
                    /* Battery icon: 12×8 at cols 110-121. */
                    jpp_icon_battery_draw(0u, 110u,
                                          bat_state.valid ? bat_state.percent : 0);
                    last_drawn_bat_pct = cur_bat_pct;

                    /* Wi-Fi icon: 8×8 at col 36. Solid=connected, blink=connecting. */
                    jpp_icon_draw(0u, 36u,
                        wifi_visible ? JPP_ICON_WIFI : JPP_ICON_NONE);
                    last_drawn_wifi = wifi_visible;
                }

                ssd1306_flush();
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(JPP_UI_REFRESH_MS));
    }
}

/* ---- Entry point -------------------------------------------------------- */

void app_main(void)
{
    /* Native app code loads into the shared static app pool (jpp_app_pool). */
    jpp_native_loader_preinit();

    /* Global heap-pressure diagnostics: logs any failed allocation and warns
       when free heap nears the WiFi-frame-alloc floor.  Start it early so it
       covers WiFi/BLE bring-up and every later allocation. */
    jpp_heap_monitor_init();

    /* Buzzer init (does drive-strength boost on GPIO3) */
    jpp_buzzer_init();

    /* Check whether CENTER is held at power-on.  If held:
       - the startup jingle is always muted (regardless of dummy mode);
       - if dummy mode is active it is also disabled.
       Sample before any display work so the check runs as early as possible. */
    s_boot_center_held = check_center_held_at_boot();
    if (s_boot_center_held) {
        ESP_LOGI(TAG, "BOOT: CENTER held — jingle muted");
    }

    /* Pre-boot: I²C + OLED for boot progress display. */
    boot_disp_t disp = { .oled_ok = false, .next_step = 0 };

    bool i2c_ok = init_i2c(JPP_HW_OLED_SDA_PIN, JPP_HW_OLED_SCL_PIN);
    if (i2c_ok) {
        disp.oled_ok = ssd1306_init(jpp_hw_init_i2c_bus(), JPP_HW_OLED_I2C_ADDR);
    }
    boot_disp_show_splash(&disp);

    /* Step 1 */
    jpp_boot_context_t boot;
    jpp_boot_context_init(&boot);
    ESP_LOGI(TAG, "BOOT_START");

    /* Step 2 */
    bool storage_ok = mount_flash_storage();
    if (!storage_ok) {
        ESP_LOGE(TAG, "Flash storage mount failed, cannot continue");
        boot_disp_step(&disp, "STORAGE", false);
        return;
    }
    jpp_boot_note_storage_ready(&boot);
    ESP_LOGI(TAG, "STORAGE_READY");
    boot_disp_step(&disp, "STORAGE", true);

    /* Step 3 */
    jpp_settings_storage_probe_t probe = {
        .main_exists    = file_exists(SETTINGS_PATH),
        .temp_exists    = file_exists(SETTINGS_TMP_PATH),
        .payload_status = JPP_SETTINGS_PAYLOAD_MISSING,
    };
    if (probe.main_exists) {
        probe.payload_status = probe_settings_payload(SETTINGS_PATH);
    } else if (probe.temp_exists) {
        probe.payload_status = probe_settings_payload(SETTINGS_TMP_PATH);
    }

    jpp_settings_load_result_t settings_result;
    jpp_settings_classify(&probe, &settings_result);

    if (settings_result.recovered_marker) {
        rename(SETTINGS_TMP_PATH, SETTINGS_PATH);
        ESP_LOGI(TAG, "SETTINGS_RECOVERED");
    }
    if (settings_result.corrupt_reset_marker) {
        write_settings(DEFAULT_SETTINGS);
        ESP_LOGI(TAG, "SETTINGS_RESET");
    }
    if (settings_result.used_defaults_marker && !settings_result.corrupt_reset_marker) {
        write_settings(DEFAULT_SETTINGS);
    }

    jpp_boot_note_settings_ready(&boot, &settings_result);
    ESP_LOGI(TAG, "SETTINGS_READY: source=%s",
             jpp_settings_source_name(settings_result.source));
    boot_disp_step(&disp, "SETTINGS", true);

    /* Step 4 */
    bool force_recovery = read_force_recovery();
    jpp_sd_config_t sd_cfg;
    jpp_sd_config_defaults(&sd_cfg);
    sd_cfg.sck  = JPP_HW_SD_SCK_PIN;
    sd_cfg.mosi = JPP_HW_SD_MOSI_PIN;
    sd_cfg.miso = JPP_HW_SD_MISO_PIN;
    sd_cfg.cs   = JPP_HW_SD_CS_PIN;
    bool sd_mounted = mount_sd(&sd_cfg);
    if (sd_mounted) {
        ESP_LOGI(TAG, "SD_MOUNT_OK");
    } else {
        ESP_LOGW(TAG, "SD_MOUNT_FAILED (recovery mode)");
    }
    jpp_boot_note_sd_mount(&boot, sd_mounted, force_recovery,
                           sd_mounted ? "ok" : "SD_MOUNT_FAILED");
    ESP_LOGI(TAG, "BOOT_MODE: %s", jpp_boot_mode_name(boot.boot_mode));
    boot_disp_step(&disp, "SD CARD", sd_mounted);

    /* Step 5 */
    jpp_boot_note_runtime_paths_ready(&boot);
    ESP_LOGI(TAG, "RUNTIME_PATHS_READY");

    /* Step 6 */
    jpp_oled_config_t oled_cfg;
    jpp_oled_config_defaults(&oled_cfg);
    jpp_oled_state_t oled_state;
    jpp_oled_state_init(&oled_state, &oled_cfg, i2c_ok);
    ESP_LOGI(TAG, "OLED: %s",
             oled_state.available
             ? "available"
             : jpp_oled_unavailable_reason_name(oled_state.unavailable_reason));

    jpp_rtc_config_t rtc_cfg;
    jpp_rtc_config_defaults(&rtc_cfg);
    rtc_cfg.i2c_bus    = i2c_ok ? jpp_hw_init_i2c_bus() : NULL;
    rtc_cfg.ds1307_addr = JPP_HW_DS1307_I2C_ADDR;
    jpp_rtc_state_t rtc_state;
    jpp_rtc_state_init(&rtc_state, &rtc_cfg, i2c_ok);
    if (rtc_state.hw_attached) {
        ESP_LOGI(TAG, "RTC: DS1307 attached");
    } else {
        ESP_LOGW(TAG, "RTC: no hardware");
    }

    /* LRV identity lives on the AT24C32 EEPROM (0x50) on the RTC breakout, not
       NVS — bind it to the shared I2C bus and load/auto-unlock from the chip. */
    jpp_lrv_init(i2c_ok ? jpp_hw_init_i2c_bus() : NULL);

    jpp_boot_note_services_ready(&boot);
    ESP_LOGI(TAG, "SERVICES_READY");

    init_wifi();

    jpp_fileserver_config_t fs_cfg;
    jpp_fileserver_config_defaults(&fs_cfg);
    jpp_fileserver_init(&fs_cfg);

    /* NVS (idempotent) */
    {
        esp_err_t nvs_err = nvs_flash_init();
        if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
            nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
    }

    jpp_ble_native_init();
    jpp_native_services_init(&rtc_state);
    jpp_serial_mgr_init();
    jpp_serial_mgr_set_rtc(&rtc_state);

    /* Load dummy mode from NVS now that NVS is initialised. */
    load_dummy_mode();

    /* If CENTER was held at boot and dummy mode is active: disable it.
       Show a brief confirmation on the OLED (already initialised above). */
    if (s_boot_center_held && s_dummy_enabled) {
        settings_do_dummy_mode_save(false, NULL);
        ssd1306_clear();
        ssd1306_draw_string(2, 20, "Dummy Mode", false);
        ssd1306_draw_string(4, 20, "DISABLED", false);
        ssd1306_flush();
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    /* Apply persisted buzzer volume before the startup chime. */
    load_buzzer_volume();

    /* Step 7 */
    jpp_ui_shell_t shell;
    jpp_ui_shell_init(&shell, jpp_boot_mode_name(boot.boot_mode));

    bool normal_mode = boot.boot_mode == JPP_BOOT_MODE_NORMAL;
    jpp_boot_discovery_summary_t disc;
    discover_apps(normal_mode, &disc, &shell);
    load_clocklines();

    jpp_boot_note_discovery_ready(&boot, &disc);
    ESP_LOGI(TAG,
             "DISCOVERY_READY: builtin=%zu sd=%zu disabled=%zu rejected=%zu total=%zu",
             disc.builtin_count, disc.sd_count,
             disc.disabled_count, disc.rejected_count, disc.total_count);
    boot_disp_step(&disp, "APPS", true);

    /* Dummy mode: push the locked app to the stack so the main loop
       launches it immediately on the first tick instead of showing the launcher. */
    if (s_dummy_enabled && s_dummy_app_id[0] != '\0') {
        jpp_ui_stack_push(&shell.stack, s_dummy_app_id);
        ESP_LOGI(TAG, "DUMMY_MODE: will launch %s on first tick", s_dummy_app_id);
    }

    /* Step 8 */
    jpp_boot_note_system_ready(&boot);
    ESP_LOGI(TAG, "SYSTEM_READY");

    /* Play startup jingle (async: the launcher comes up while it plays).
       Muted when CENTER was held at boot — always, regardless of dummy mode. */
    if (!s_boot_center_held) {
        jpp_buzzer_play_startup_jingle_async((jpp_startup_jingle_t)s_startup_jingle);
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    run_main_loop(&shell, &rtc_state, &boot, sd_mounted);
}
