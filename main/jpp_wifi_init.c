#include "jpp_wifi_init.h"
#include "jpp_settings_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "cJSON.h"

static const char *TAG = "wifi_init";

#define WIFI_MAX_RECONNECT_ATTEMPTS 10

/* ---- Internal state ------------------------------------------------------- */

static bool               s_wifi_started        = false;
static bool               s_handlers_registered = false;

/* Set true when the user (or init_wifi) wants the connection maintained;
   set false before a user-initiated disconnect or during blocking connect. */
static volatile bool      s_should_reconnect    = false;

/* Set true while wifi_connect() is waiting for the result. */
static volatile bool      s_connecting          = false;

/* Consecutive failed auto-reconnect attempts; capped at WIFI_MAX_RECONNECT_ATTEMPTS. */
static int                s_reconnect_attempts  = 0;

/* True once we have a valid IP address. */
static volatile bool      s_has_ip              = false;

/* SSID we are currently associated with (empty when not connected). */
static char               s_connected_ssid[33]  = {0};

/* Event group used by the blocking wifi_connect(). */
static EventGroupHandle_t s_event_group         = NULL;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* ---- Event handler -------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch ((wifi_event_t)id) {
        case WIFI_EVENT_STA_DISCONNECTED:
            s_has_ip            = false;
            s_connected_ssid[0] = '\0';
            if (s_connecting && s_event_group) {
                /* Blocking wifi_connect() is waiting — signal failure. */
                xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
            } else if (s_should_reconnect) {
                s_reconnect_attempts++;
                if (s_reconnect_attempts >= WIFI_MAX_RECONNECT_ATTEMPTS) {
                    s_should_reconnect   = false;
                    s_reconnect_attempts = 0;
                    ESP_LOGW(TAG, "WIFI: giving up after %d failed attempts",
                             WIFI_MAX_RECONNECT_ATTEMPTS);
                } else {
                    ESP_LOGW(TAG, "WIFI_DISCONNECTED — retrying (%d/%d)",
                             s_reconnect_attempts, WIFI_MAX_RECONNECT_ATTEMPTS);
                    esp_wifi_connect();
                }
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WIFI_GOT_IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_has_ip             = true;
        s_reconnect_attempts = 0;

        /* Cache the associated SSID. */
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            strncpy(s_connected_ssid, (char *)ap.ssid,
                    sizeof(s_connected_ssid) - 1u);
            s_connected_ssid[sizeof(s_connected_ssid) - 1u] = '\0';
        }

        /* Maintain the connection from now on. */
        s_should_reconnect = true;

        if (s_connecting && s_event_group) {
            /* Signal the blocking wifi_connect() that we are up. */
            s_connecting = false;
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

/* ---- Credential helper ---------------------------------------------------- */

static void read_wifi_policy(char *ssid,     size_t ssid_size,
                              char *password, size_t password_size)
{
    ssid[0]     = '\0';
    password[0] = '\0';

    FILE *f = fopen(SETTINGS_PATH, "r");
    if (f == NULL) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)size + 1u);
    if (buf == NULL) { fclose(f); return; }
    fread(buf, 1u, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);

    cJSON *root   = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) return;

    cJSON *policy = cJSON_GetObjectItem(root, "policy");
    cJSON *wifi   = policy ? cJSON_GetObjectItem(policy, "wifi") : NULL;
    cJSON *j_ssid = wifi   ? cJSON_GetObjectItem(wifi, "preferred_ssid") : NULL;
    cJSON *j_pass = wifi   ? cJSON_GetObjectItem(wifi, "password")       : NULL;

    if (cJSON_IsString(j_ssid) && j_ssid->valuestring) {
        strncpy(ssid, j_ssid->valuestring, ssid_size - 1u);
        ssid[ssid_size - 1u] = '\0';
    }
    if (cJSON_IsString(j_pass) && j_pass->valuestring) {
        strncpy(password, j_pass->valuestring, password_size - 1u);
        password[password_size - 1u] = '\0';
    }
    cJSON_Delete(root);
}

/* ---- Public API ----------------------------------------------------------- */

bool wifi_ensure_started(void)
{
    if (s_wifi_started) return true;

    /* NVS — idempotent. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* TCP/IP stack and event loop — idempotent. */
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    /* Wi-Fi driver. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t init_err = esp_wifi_init(&cfg);
    if (init_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_init returned %s (may already be running)",
                 esp_err_to_name(init_err));
    }

    esp_wifi_set_mode(WIFI_MODE_STA);

    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(start_err));
        return false;
    }

    s_wifi_started = true;

    /* Disable modem power-save.  The default WIFI_PS_MIN_MODEM makes the radio
       doze between AP beacons and send "null data frames" to the AP on every
       sleep/wake transition.  Under a busy TCP server (WebDAV) the driver fails
       to allocate those null frames during a burst — logged as "wifi:m f null" —
       which stalls the link, resets sockets (recv errno 104), and eventually
       wedges Wi-Fi entirely.  WIFI_PS_NONE keeps the radio always-on: no null
       frames, no doze-induced packet loss, stable sockets during transfers.
       Acceptable trade-off for a mains/desk-powered device serving files. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Register event handlers once. */
    if (!s_handlers_registered) {
        esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
        esp_event_handler_instance_register(
            IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);
        s_handlers_registered = true;
    }

    /* Event group for blocking connects. */
    if (s_event_group == NULL) {
        s_event_group = xEventGroupCreate();
    }

    return true;
}

bool init_wifi(void)
{
    char ssid[64]     = {0};
    char password[64] = {0};
    read_wifi_policy(ssid, sizeof(ssid), password, sizeof(password));

    if (ssid[0] == '\0') {
        ESP_LOGI(TAG, "WIFI: no SSID configured, skipping");
        return false;
    }

    if (!wifi_ensure_started()) return false;

    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    strncpy((char *)wifi_cfg.sta.ssid,     ssid,     sizeof(wifi_cfg.sta.ssid) - 1u);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1u);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

    /* Auto-reconnect: maintain connection if it drops. */
    s_reconnect_attempts = 0;
    s_should_reconnect   = true;
    esp_wifi_connect();

    ESP_LOGI(TAG, "WIFI: connecting to \"%s\" (non-blocking)", ssid);
    return true;
}

bool wifi_connect(const char *ssid, const char *password,
                  uint32_t timeout_ms,
                  char *out_err_msg, size_t err_msg_len)
{
    if (out_err_msg && err_msg_len > 0u) out_err_msg[0] = '\0';

    if (!wifi_ensure_started()) {
        if (out_err_msg) snprintf(out_err_msg, err_msg_len, "Wi-Fi unavailable");
        return false;
    }

    /* Stop any ongoing auto-reconnect loop and disconnect. */
    s_should_reconnect   = false;
    s_connecting         = false;
    s_reconnect_attempts = 0;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));  /* let DISCONNECTED event settle */

    /* Clear event bits from any previous attempt. */
    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    /* Configure the target network. */
    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1u);
    if (password && password[0] != '\0') {
        strncpy((char *)wifi_cfg.sta.password, password,
                sizeof(wifi_cfg.sta.password) - 1u);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

    /* Mark that we are in a blocking connect attempt. */
    s_connecting = true;
    esp_wifi_connect();

    ESP_LOGI(TAG, "WIFI: connecting to \"%s\" (blocking, timeout=%lums)",
             ssid, (unsigned long)timeout_ms);

    /* Wait for success or failure. */
    EventBits_t bits = xEventGroupWaitBits(
        s_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,   /* do not clear bits on exit */
        pdFALSE,   /* wake on any one bit */
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WIFI: connected to \"%s\"", ssid);
        return true;
    }

    /* Failure or timeout — clean up. */
    s_connecting       = false;
    s_should_reconnect = false;

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "WIFI: auth failed for \"%s\"", ssid);
        if (out_err_msg) snprintf(out_err_msg, err_msg_len, "Auth failed");
    } else {
        ESP_LOGW(TAG, "WIFI: timed out connecting to \"%s\"", ssid);
        if (out_err_msg) snprintf(out_err_msg, err_msg_len, "Timed out");
        esp_wifi_disconnect();
    }
    return false;
}

void wifi_disconnect(void)
{
    s_should_reconnect   = false;
    s_connecting         = false;
    s_reconnect_attempts = 0;
    s_has_ip             = false;
    s_connected_ssid[0]  = '\0';
    esp_wifi_disconnect();
    ESP_LOGI(TAG, "WIFI: disconnected by user");
}

bool wifi_is_connected(void)
{
    return s_has_ip;
}

bool wifi_get_connected_ssid(char *out, size_t out_len)
{
    if (!s_has_ip || s_connected_ssid[0] == '\0') {
        if (out && out_len > 0u) out[0] = '\0';
        return false;
    }
    if (out && out_len > 0u) {
        strncpy(out, s_connected_ssid, out_len - 1u);
        out[out_len - 1u] = '\0';
    }
    return true;
}

bool wifi_get_saved_ssid(char *out, size_t out_len)
{
    if (!out || out_len == 0u) { return false; }
    out[0] = '\0';
    char ssid[64] = {0}, pass[2] = {0};
    read_wifi_policy(ssid, sizeof(ssid), pass, sizeof(pass));
    if (ssid[0] == '\0') { return false; }
    strncpy(out, ssid, out_len - 1u);
    out[out_len - 1u] = '\0';
    return true;
}

bool wifi_is_connecting(void)
{
    return s_should_reconnect && !s_has_ip;
}
