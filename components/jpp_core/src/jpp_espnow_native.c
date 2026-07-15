#include "../include/jpp_espnow_native.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

/*
 * jpp_core cannot depend on main/ (jpp_ble_native.c sets the precedent: it
 * manages its own NimBLE stack directly rather than reaching into main/).
 * This module brings up the WiFi driver itself, the same idempotent way
 * main/jpp_wifi_init.c's wifi_ensure_started() does — both call the same
 * ESP-IDF init functions, which tolerate "already running".
 */

static const char *TAG = "jpp_espnow";

#define ESPNOW_RX_QUEUE_DEPTH 8u
#define ESPNOW_SEND_TIMEOUT_MS 1000u

typedef struct {
    uint8_t mac[6];
    uint8_t data[JPP_SDK_ESPNOW_DATA_MAX];
    size_t  len;
} espnow_rx_item_t;

static bool               s_wifi_up      = false;
static bool               s_espnow_ready = false;
static QueueHandle_t      s_rx_queue     = NULL;
static SemaphoreHandle_t  s_send_sem     = NULL;
static volatile esp_now_send_status_t s_last_send_status = ESP_NOW_SEND_FAIL;

static bool espnow_wifi_ensure_started(void)
{
    if (s_wifi_up) {
        return true;
    }

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

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

    s_wifi_up = true;
    return true;
}

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int data_len)
{
    if (info == NULL || info->src_addr == NULL || data == NULL || data_len <= 0 ||
        s_rx_queue == NULL) {
        return;
    }
    espnow_rx_item_t item;
    memcpy(item.mac, info->src_addr, 6u);
    size_t len = (size_t)data_len;
    if (len > JPP_SDK_ESPNOW_DATA_MAX) {
        len = JPP_SDK_ESPNOW_DATA_MAX;
    }
    memcpy(item.data, data, len);
    item.len = len;
    /* Best-effort: drop the packet if the app hasn't drained the queue. */
    xQueueSend(s_rx_queue, &item, 0);
}

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    s_last_send_status = status;
    if (s_send_sem != NULL) {
        xSemaphoreGive(s_send_sem);
    }
}

static bool espnow_ensure_ready(void)
{
    if (s_espnow_ready) {
        return true;
    }
    if (!espnow_wifi_ensure_started()) {
        return false;
    }
    if (esp_now_init() != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_init failed");
        return false;
    }
    esp_now_register_recv_cb(espnow_recv_cb);
    esp_now_register_send_cb(espnow_send_cb);

    s_rx_queue = xQueueCreate(ESPNOW_RX_QUEUE_DEPTH, sizeof(espnow_rx_item_t));
    s_send_sem = xSemaphoreCreateBinary();
    if (s_rx_queue == NULL || s_send_sem == NULL) {
        ESP_LOGW(TAG, "espnow queue/semaphore alloc failed");
        return false;
    }

    s_espnow_ready = true;
    return true;
}

/* ---- Service implementations ---------------------------------------------- */

static jpp_broker_status_t espnow_send_impl(void *context, const uint8_t peer_mac[6],
                                             const uint8_t *data, size_t data_len,
                                             jpp_broker_result_t *result)
{
    (void)context;
    if (peer_mac == NULL || data == NULL || result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    if (data_len > JPP_SDK_ESPNOW_DATA_MAX) {
        jpp_broker_error_result(result, "TOO_LARGE");
        return JPP_BROKER_STATUS_OK;
    }
    if (!espnow_ensure_ready()) {
        jpp_broker_error_result(result, "ESPNOW_INIT_FAILED");
        return JPP_BROKER_STATUS_OK;
    }

    if (!esp_now_is_peer_exist(peer_mac)) {
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, peer_mac, 6u);
        peer.channel = 0u;         /* current WiFi channel */
        peer.ifidx   = WIFI_IF_STA;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            jpp_broker_error_result(result, "ESPNOW_PEER_ADD_FAILED");
            return JPP_BROKER_STATUS_OK;
        }
    }

    xSemaphoreTake(s_send_sem, 0);  /* drain any stale signal */
    if (esp_now_send(peer_mac, data, data_len) != ESP_OK) {
        jpp_broker_error_result(result, "ESPNOW_SEND_FAILED");
        return JPP_BROKER_STATUS_OK;
    }

    if (xSemaphoreTake(s_send_sem, pdMS_TO_TICKS(ESPNOW_SEND_TIMEOUT_MS)) != pdTRUE) {
        jpp_broker_error_result(result, "ESPNOW_SEND_TIMEOUT");
        return JPP_BROKER_STATUS_OK;
    }
    if (s_last_send_status != ESP_NOW_SEND_SUCCESS) {
        jpp_broker_error_result(result, "ESPNOW_SEND_FAILED");
        return JPP_BROKER_STATUS_OK;
    }

    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

static jpp_broker_status_t espnow_recv_impl(void *context, uint8_t out_peer_mac[6],
                                             uint8_t *out_data, size_t max_len,
                                             size_t *out_len, uint32_t timeout_ms,
                                             jpp_broker_result_t *result)
{
    (void)context;
    if (out_peer_mac == NULL || out_data == NULL || out_len == NULL || result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    if (!espnow_ensure_ready()) {
        jpp_broker_error_result(result, "ESPNOW_INIT_FAILED");
        return JPP_BROKER_STATUS_OK;
    }

    espnow_rx_item_t item;
    if (xQueueReceive(s_rx_queue, &item, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        jpp_broker_error_result(result, "NO_DATA");
        return JPP_BROKER_STATUS_OK;
    }

    memcpy(out_peer_mac, item.mac, 6u);
    size_t copy_len = (item.len < max_len) ? item.len : max_len;
    memcpy(out_data, item.data, copy_len);
    *out_len = copy_len;

    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

void jpp_espnow_native_get_services(jpp_sdk_espnow_send_fn_t *out_send,
                                     jpp_sdk_espnow_recv_fn_t *out_recv,
                                     void **out_context)
{
    if (out_send != NULL)    { *out_send = espnow_send_impl; }
    if (out_recv != NULL)    { *out_recv = espnow_recv_impl; }
    if (out_context != NULL) { *out_context = NULL; }
}
