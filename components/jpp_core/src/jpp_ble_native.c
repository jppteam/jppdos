/*
 * jpp_ble_native.c — NimBLE host integration for JPP SDK BLE services.
 *
 * Architecture:
 *   - The NimBLE stack runs its own host task (started by nimble_port_init +
 *     nimble_port_freertos_init/nimble_port_run). nimble_port_init() performs
 *     controller and HCI initialisation internally (ESP-IDF >= 5.1).
 *   - Each synchronous SDK call drives async NimBLE operations; a FreeRTOS
 *     binary semaphore is used to block the calling task until the NimBLE
 *     callback fires.
 *   - A static table (s_conns[]) tracks up to JPP_SDK_BLE_CONN_CAPACITY active
 *     GATT client connections, keyed by address string.
 *   - Only one scan or advertise operation may be in flight at a time; the SDK
 *     broker's "ble_radio" exclusive lock enforces this at a higher layer.
 *
 * Generic app-host GATT service (ble.host):
 *   - A single static NimBLE GATT service is pre-registered before the host
 *     starts and shared by whichever app claims the host role (apps run
 *     serialized). The protocol carried over it is entirely app-defined.
 *   - TX characteristic (readable): bytes the app publishes for a connected peer
 *     to read.  Updated via jpp_ble_host_set_tx() (SDK: jpp_sdk_ble_host_set_value).
 *   - RX characteristic (writable): bytes a connected peer writes to the app.
 *     The app waits on jpp_ble_host_wait_rx() (SDK: jpp_sdk_ble_host_wait_write).
 *   - UUIDs mirror JPP_SDK_BLE_HOST_*_UUID:
 *       Service:  4a505300-0000-0000-0000-000000000000
 *       TX char:  4a505301-0000-0000-0000-000000000000
 *       RX char:  4a505302-0000-0000-0000-000000000000
 */

#include "../include/jpp_ble_native.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#ifdef CONFIG_BT_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/ble.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"
#endif /* CONFIG_BT_ENABLED */

/* In Wokwi simulation the BLE controller emulation is incomplete and causes an
 * interrupt WDT crash inside nimble_port_run().  Treat JPP_WOKWI_SIM as if BT
 * were disabled so the no-op stubs are compiled in instead. */
#if defined(CONFIG_BT_ENABLED) && !defined(JPP_WOKWI_SIM)

static const char *TAG = "jpp_ble_native";

/* ---- Internal connection table ------------------------------------------ */

typedef struct {
    char     address[JPP_SDK_BLE_ADDR_LEN]; /* "AA:BB:CC:DD:EE:FF\0" */
    uint16_t handle;
    uint16_t rx_char_handle; /* cached value handle for the app-host RX char */
    bool     active;
} ble_conn_slot_t;

static ble_conn_slot_t s_conns[JPP_SDK_BLE_CONN_CAPACITY];

/* ---- Synchronisation primitives ------------------------------------------ */

/* One shared semaphore for serialising async callbacks back to callers.
   The SDK broker's exclusive lock ensures only one BLE op runs at a time.
   s_op_guard validates this contract at runtime in debug builds. */
static SemaphoreHandle_t s_ble_done;

#ifndef NDEBUG
static volatile bool s_op_in_progress = false;
#define BLE_OP_ENTER() do { \
    configASSERT(!s_op_in_progress && "concurrent BLE op — broker lock missing"); \
    s_op_in_progress = true; \
} while (0)
#define BLE_OP_EXIT()  do { s_op_in_progress = false; } while (0)
#else
#define BLE_OP_ENTER() ((void)0)
#define BLE_OP_EXIT()  ((void)0)
#endif

/* Shared result storage written by callbacks before giving s_ble_done. */
static int               s_ble_status;
static uint8_t           s_read_buf[2048];
static size_t            s_read_len;
static uint16_t          s_discovered_handle; /* temp: filled by disc_chr_cb */

/* ---- Connectable advertising flag ---------------------------------------- */

static bool s_connectable = false;

/* ---- Stack active state -------------------------------------------------- */

/* True once the NimBLE stack (controller + host task) is running.  Cleared by
   jpp_ble_native_suspend() so the controller's heap is released while WiFi needs
   the headroom (e.g. WebDAV transfers), and restored by jpp_ble_native_resume(). */
static bool s_ble_active = false;

void jpp_ble_native_set_connectable(bool connectable)
{
    s_connectable = connectable;
}

/* ---- GATT host service globals ------------------------------------------ */

/* TX buffer: app fills this; GATT clients read it. */
static uint8_t           s_tx_buf[JPP_BLE_HOST_TX_BUF_SIZE];
static size_t            s_tx_len;
static SemaphoreHandle_t s_tx_mutex;

/* RX buffer: GATT clients write here; app reads via wait_rx. */
static uint8_t           s_rx_buf[JPP_BLE_HOST_RX_BUF_SIZE];
static size_t            s_rx_len;
static SemaphoreHandle_t s_rx_mutex;
static SemaphoreHandle_t s_rx_ready; /* counting semaphore, max 2 */

/* GATT service/characteristic handle tracking. */
static uint16_t s_gatt_svc_handle;

/* ---- Address utilities --------------------------------------------------- */

static void addr_to_str(const uint8_t *addr, char *out)
{
    snprintf(out, JPP_SDK_BLE_ADDR_LEN,
             "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static bool str_to_addr(const char *str, uint8_t *out)
{
    unsigned int b[6];
    if (sscanf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    out[5] = (uint8_t)b[0];
    out[4] = (uint8_t)b[1];
    out[3] = (uint8_t)b[2];
    out[2] = (uint8_t)b[3];
    out[1] = (uint8_t)b[4];
    out[0] = (uint8_t)b[5];
    return true;
}

/* ---- Connection table helpers -------------------------------------------- */

static ble_conn_slot_t *find_conn_by_addr(const char *address)
{
    for (int i = 0; i < JPP_SDK_BLE_CONN_CAPACITY; i++) {
        if (s_conns[i].active &&
            strncmp(s_conns[i].address, address, JPP_SDK_BLE_ADDR_LEN) == 0) {
            return &s_conns[i];
        }
    }
    return NULL;
}

static ble_conn_slot_t *find_free_conn_slot(void)
{
    for (int i = 0; i < JPP_SDK_BLE_CONN_CAPACITY; i++) {
        if (!s_conns[i].active) {
            return &s_conns[i];
        }
    }
    return NULL;
}

/* ---- Scan ---------------------------------------------------------------- */

typedef struct {
    jpp_sdk_ble_scan_result_t *results;
    size_t max_results;
    size_t count;
} scan_ctx_t;

static scan_ctx_t s_scan_ctx;

static int scan_event_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }
    if (s_scan_ctx.count >= s_scan_ctx.max_results) {
        return 0;
    }

    jpp_sdk_ble_scan_result_t *r = &s_scan_ctx.results[s_scan_ctx.count];
    addr_to_str(event->disc.addr.val, r->address);
    r->rssi = event->disc.rssi;
    r->name[0] = '\0';

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                event->disc.length_data) == 0) {
        if (fields.name != NULL && fields.name_len > 0u) {
            size_t copy = fields.name_len < (JPP_SDK_BLE_NAME_MAX - 1u)
                        ? fields.name_len
                        : (JPP_SDK_BLE_NAME_MAX - 1u);
            memcpy(r->name, fields.name, copy);
            r->name[copy] = '\0';
        }
    }

    size_t ad_len = event->disc.length_data;
    if (ad_len > JPP_SDK_BLE_AD_PAYLOAD_MAX) {
        ad_len = JPP_SDK_BLE_AD_PAYLOAD_MAX;
    }
    memcpy(r->ad_data, event->disc.data, ad_len);
    r->ad_data_len = ad_len;

    s_scan_ctx.count++;
    return 0;
}

static int scan_complete_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_ble_status = 0;
        xSemaphoreGive(s_ble_done);
    }
    return scan_event_cb(event, arg);
}

static jpp_broker_status_t ble_scan_impl(
    void *context,
    uint32_t duration_ms,
    jpp_sdk_ble_scan_result_t *results,
    size_t max_results,
    size_t *out_count,
    jpp_broker_result_t *result
)
{
    (void)context;

    if (results == NULL || out_count == NULL || result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }

    BLE_OP_ENTER();

    memset(&s_scan_ctx, 0, sizeof(s_scan_ctx));
    s_scan_ctx.results     = results;
    s_scan_ctx.max_results = max_results;
    s_scan_ctx.count       = 0u;
    s_ble_status = -1;

    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.passive           = 1;
    params.filter_duplicates = 1;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC,
                          (int32_t)duration_ms,
                          &params,
                          scan_complete_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc failed: %d", rc);
        jpp_broker_error_result(result, "BLE_SCAN_START_FAILED");
        BLE_OP_EXIT();
        return JPP_BROKER_STATUS_OK;
    }

    xSemaphoreTake(s_ble_done, portMAX_DELAY);

    *out_count = s_scan_ctx.count;
    jpp_broker_ok_result(result);
    BLE_OP_EXIT();
    return JPP_BROKER_STATUS_OK;
}

/* ---- Advertise ----------------------------------------------------------- */

static jpp_broker_status_t ble_advertise_start_impl(
    void *context,
    const uint8_t *payload,
    size_t payload_len,
    jpp_broker_result_t *result
)
{
    (void)context;

    if (result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }

    int rc = ble_gap_adv_set_data(payload, (uint8_t)payload_len);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_adv_set_data failed: %d", rc);
        jpp_broker_error_result(result, "BLE_ADV_SET_DATA_FAILED");
        return JPP_BROKER_STATUS_OK;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = s_connectable
                         ? BLE_GAP_CONN_MODE_UND   /* undirected connectable */
                         : BLE_GAP_CONN_MODE_NON;  /* non-connectable        */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, NULL, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_adv_start failed: %d", rc);
        jpp_broker_error_result(result, "BLE_ADV_START_FAILED");
        return JPP_BROKER_STATUS_OK;
    }

    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

static jpp_broker_status_t ble_advertise_stop_impl(
    void *context,
    jpp_broker_result_t *result
)
{
    (void)context;

    if (result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }

    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_adv_stop failed: %d", rc);
        jpp_broker_error_result(result, "BLE_ADV_STOP_FAILED");
        return JPP_BROKER_STATUS_OK;
    }

    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

/* ---- Connect ------------------------------------------------------------- */

static int connect_event_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        s_ble_status = event->connect.status;
        if (s_ble_status == 0) {
            ble_conn_slot_t *slot = (ble_conn_slot_t *)arg;
            if (slot != NULL) {
                slot->handle = event->connect.conn_handle;
            }
        }
        xSemaphoreGive(s_ble_done);
    }
    return 0;
}

/* Signals completion of an ATT MTU exchange initiated in ble_connect_impl. */
static int mtu_exchange_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           uint16_t mtu, void *arg)
{
    (void)conn_handle;
    (void)arg;
    s_ble_status = (error != NULL) ? error->status : 0;
    if (s_ble_status == 0) {
        ESP_LOGD(TAG, "ATT MTU negotiated: %u", mtu);
    }
    xSemaphoreGive(s_ble_done);
    return 0;
}

static jpp_broker_status_t ble_connect_impl(
    void *context,
    const char *address,
    jpp_broker_result_t *result
)
{
    (void)context;

    if (address == NULL || result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }

    ble_conn_slot_t *slot = find_free_conn_slot();
    if (slot == NULL) {
        jpp_broker_error_result(result, "BLE_CONN_LIMIT");
        return JPP_BROKER_STATUS_OK;
    }

    uint8_t addr_bytes[6];
    if (!str_to_addr(address, addr_bytes)) {
        jpp_broker_error_result(result, "BLE_INVALID_ADDRESS");
        return JPP_BROKER_STATUS_OK;
    }

    BLE_OP_ENTER();

    ble_addr_t peer_addr;
    peer_addr.type = BLE_ADDR_PUBLIC;
    memcpy(peer_addr.val, addr_bytes, 6);

    s_ble_status = -1;
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer_addr,
                             10000,
                             NULL, connect_event_cb, slot);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect failed: %d", rc);
        jpp_broker_error_result(result, "BLE_CONNECT_FAILED");
        BLE_OP_EXIT();
        return JPP_BROKER_STATUS_OK;
    }

    xSemaphoreTake(s_ble_done, portMAX_DELAY);

    if (s_ble_status != 0) {
        ESP_LOGW(TAG, "BLE connect error: %d", s_ble_status);
        jpp_broker_error_result(result, "BLE_CONNECT_ERROR");
        BLE_OP_EXIT();
        return JPP_BROKER_STATUS_OK;
    }

    /* Negotiate a larger ATT MTU.  The default (23) caps a characteristic
       read/write at ~20 bytes, which silently truncates MeetApp's multi-byte
       payloads (97-byte nonce blob, 33-byte partial sig, etc.) — the leader
       then rejects the short read and signs over a garbage peer pubkey,
       surfacing as "Could not create partial signature".  The preferred MTU is
       CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU (256); NimBLE does not auto-initiate
       the exchange, so the central must request it here.  Best-effort: on
       failure we continue at the default MTU rather than fail the connect. */
    s_ble_status = -1;
    int mrc = ble_gattc_exchange_mtu(slot->handle, mtu_exchange_cb, NULL);
    if (mrc == 0) {
        xSemaphoreTake(s_ble_done, portMAX_DELAY);
        if (s_ble_status != 0) {
            ESP_LOGW(TAG, "MTU exchange failed: %d (using default MTU)", s_ble_status);
        }
    } else if (mrc != BLE_HS_EALREADY) {
        /* EALREADY = peer already initiated the exchange; MTU is set. */
        ESP_LOGW(TAG, "ble_gattc_exchange_mtu init failed: %d", mrc);
    }

    strncpy(slot->address, address, JPP_SDK_BLE_ADDR_LEN - 1u);
    slot->address[JPP_SDK_BLE_ADDR_LEN - 1u] = '\0';
    slot->rx_char_handle = 0;
    slot->active         = true;

    jpp_broker_ok_result(result);
    BLE_OP_EXIT();
    return JPP_BROKER_STATUS_OK;
}

/* ---- Read characteristic ------------------------------------------------- */

static int read_char_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (error->status == 0 && attr != NULL) {
        s_read_len = OS_MBUF_PKTLEN(attr->om);
        if (s_read_len > sizeof(s_read_buf)) {
            s_read_len = sizeof(s_read_buf);
        }
        uint16_t read_copied = 0u;
        ble_hs_mbuf_to_flat(attr->om, s_read_buf, (uint16_t)s_read_len, &read_copied);
        s_read_len = read_copied;
    } else {
        s_read_len = 0u;
    }
    s_ble_status = error->status;
    xSemaphoreGive(s_ble_done);
    return 0;
}

static bool parse_uuid(const char *str, ble_uuid_any_t *out)
{
    uint8_t b[16];
    unsigned int v[16];
    if (strlen(str) == 36 &&
        sscanf(str,
               "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
               &v[0],  &v[1],  &v[2],  &v[3],
               &v[4],  &v[5],  &v[6],  &v[7],
               &v[8],  &v[9],  &v[10], &v[11],
               &v[12], &v[13], &v[14], &v[15]) == 16) {
        for (int i = 0; i < 16; i++) {
            b[i] = (uint8_t)v[i];
        }
        out->u128.u.type = BLE_UUID_TYPE_128;
        for (int i = 0; i < 16; i++) {
            out->u128.value[i] = b[15 - i];
        }
        return true;
    }
    unsigned int u16;
    if (sscanf(str, "%04x", &u16) == 1) {
        out->u16.u.type = BLE_UUID_TYPE_16;
        out->u16.value  = (uint16_t)u16;
        return true;
    }
    return false;
}

static jpp_broker_status_t ble_read_char_impl(
    void *context,
    const char *address,
    const char *svc_uuid,
    const char *char_uuid,
    jpp_broker_result_t *result
)
{
    (void)context;
    (void)svc_uuid;

    if (address == NULL || char_uuid == NULL || result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }

    ble_conn_slot_t *slot = find_conn_by_addr(address);
    if (slot == NULL) {
        jpp_broker_error_result(result, "BLE_NOT_CONNECTED");
        return JPP_BROKER_STATUS_OK;
    }

    ble_uuid_any_t c_uuid;
    if (!parse_uuid(char_uuid, &c_uuid)) {
        jpp_broker_error_result(result, "BLE_INVALID_UUID");
        return JPP_BROKER_STATUS_OK;
    }

    BLE_OP_ENTER();

    s_ble_status = -1;
    s_read_len   = 0u;

    int rc = ble_gattc_read_by_uuid(slot->handle,
                                    0x0001, 0xffff,
                                    &c_uuid.u,
                                    read_char_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gattc_read_by_uuid failed: %d", rc);
        jpp_broker_error_result(result, "BLE_READ_FAILED");
        BLE_OP_EXIT();
        return JPP_BROKER_STATUS_OK;
    }

    xSemaphoreTake(s_ble_done, portMAX_DELAY);

    if (s_ble_status != 0) {
        jpp_broker_error_result(result, "BLE_READ_ERROR");
        BLE_OP_EXIT();
        return JPP_BROKER_STATUS_OK;
    }

    /* Hex-encode result into a static buffer (up to 512 bytes = 1024 hex chars). */
    static char hex_buf[1025];
    size_t encode_len = s_read_len < 512u ? s_read_len : 512u;
    for (size_t i = 0u; i < encode_len; i++) {
        snprintf(&hex_buf[i * 2u], 3u, "%02X", s_read_buf[i]);
    }
    hex_buf[encode_len * 2u] = '\0';

    jpp_broker_ok_result(result);
    jpp_broker_result_put(result, "text", hex_buf);
    BLE_OP_EXIT();
    return JPP_BROKER_STATUS_OK;
}

/* ---- Write characteristic ------------------------------------------------ */

static int write_char_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;

    s_ble_status = error->status;
    xSemaphoreGive(s_ble_done);
    return 0;
}

static int disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (error->status == 0 && chr != NULL) {
        s_discovered_handle = chr->val_handle;
        /* Don't give semaphore yet — callback fires once per char found plus
           once more with BLE_HS_EDONE to signal completion. */
    } else {
        s_ble_status = (error->status == BLE_HS_EDONE) ? 0 : (int)error->status;
        xSemaphoreGive(s_ble_done);
    }
    return 0;
}

static int hex_decode(const char *hex, uint8_t *out, size_t out_max)
{
    size_t len = strlen(hex);
    if (len % 2u != 0u) {
        return -1;
    }
    size_t bytes = len / 2u;
    if (bytes > out_max) {
        bytes = out_max;
    }
    for (size_t i = 0u; i < bytes; i++) {
        unsigned int byte;
        if (sscanf(&hex[i * 2u], "%02x", &byte) != 1) {
            return -1;
        }
        out[i] = (uint8_t)byte;
    }
    return (int)bytes;
}

static jpp_broker_status_t ble_write_char_impl(
    void *context,
    const char *address,
    const char *svc_uuid,
    const char *char_uuid,
    const char *text,
    jpp_broker_result_t *result
)
{
    (void)context;
    (void)svc_uuid;

    if (address == NULL || char_uuid == NULL || text == NULL || result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }

    ble_conn_slot_t *slot = find_conn_by_addr(address);
    if (slot == NULL) {
        jpp_broker_error_result(result, "BLE_NOT_CONNECTED");
        return JPP_BROKER_STATUS_OK;
    }

    ble_uuid_any_t c_uuid;
    if (!parse_uuid(char_uuid, &c_uuid)) {
        jpp_broker_error_result(result, "BLE_INVALID_UUID");
        return JPP_BROKER_STATUS_OK;
    }

    /* Decode hex payload. */
    static uint8_t write_buf[4096];
    int write_len = hex_decode(text, write_buf, sizeof(write_buf));
    if (write_len < 0) {
        jpp_broker_error_result(result, "BLE_WRITE_INVALID_DATA");
        return JPP_BROKER_STATUS_OK;
    }

    BLE_OP_ENTER();

    /* Discover the characteristic value handle on first write to this peer. */
    if (slot->rx_char_handle == 0) {
        s_discovered_handle = 0;
        s_ble_status = -1;
        int rc = ble_gattc_disc_chrs_by_uuid(slot->handle,
                                              0x0001, 0xffff,
                                              &c_uuid.u,
                                              disc_chr_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gattc_disc_chrs_by_uuid failed: %d", rc);
            jpp_broker_error_result(result, "BLE_DISC_FAILED");
            BLE_OP_EXIT();
            return JPP_BROKER_STATUS_OK;
        }
        xSemaphoreTake(s_ble_done, portMAX_DELAY);
        if (s_ble_status != 0 || s_discovered_handle == 0) {
            ESP_LOGW(TAG, "Characteristic discovery failed: st=%d h=%u",
                     s_ble_status, s_discovered_handle);
            jpp_broker_error_result(result, "BLE_DISC_ERROR");
            BLE_OP_EXIT();
            return JPP_BROKER_STATUS_OK;
        }
        slot->rx_char_handle = s_discovered_handle;
    }

    s_ble_status = -1;

    /* A single ATT Write Request carries at most (ATT_MTU - 3) value bytes.
       MeetApp's round-1.5 payload grows with participant count (it carries every
       signer's pubkey) and exceeds that for larger sessions, so fall back to a
       queued long write (Prepare/Execute) when it does not fit — the peer's
       NimBLE GATT server reassembles it automatically before delivering it to
       the access callback. */
    uint16_t mtu = ble_att_mtu(slot->handle);
    if (mtu < 23u) {
        mtu = 23u;  /* default if the link somehow has no MTU yet */
    }
    int rc;
    if ((size_t)write_len <= (size_t)(mtu - 3u)) {
        rc = ble_gattc_write_flat(slot->handle,
                                  slot->rx_char_handle,
                                  write_buf, (uint16_t)write_len,
                                  write_char_cb, NULL);
    } else {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(write_buf, (uint16_t)write_len);
        if (om == NULL) {
            jpp_broker_error_result(result, "BLE_WRITE_NO_MEM");
            BLE_OP_EXIT();
            return JPP_BROKER_STATUS_OK;
        }
        /* ble_gattc_write_long consumes the mbuf on both success and failure. */
        rc = ble_gattc_write_long(slot->handle, slot->rx_char_handle, 0,
                                  om, write_char_cb, NULL);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gattc write failed: %d (len=%d mtu=%u)", rc, write_len, mtu);
        jpp_broker_error_result(result, "BLE_WRITE_FAILED");
        BLE_OP_EXIT();
        return JPP_BROKER_STATUS_OK;
    }

    xSemaphoreTake(s_ble_done, portMAX_DELAY);

    if (s_ble_status != 0) {
        jpp_broker_error_result(result, "BLE_WRITE_ERROR");
        BLE_OP_EXIT();
        return JPP_BROKER_STATUS_OK;
    }

    jpp_broker_ok_result(result);
    BLE_OP_EXIT();
    return JPP_BROKER_STATUS_OK;
}

/* ---- Disconnect ---------------------------------------------------------- */

static jpp_broker_status_t ble_disconnect_impl(
    void *context,
    const char *address,
    jpp_broker_result_t *result
)
{
    (void)context;

    if (address == NULL || result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }

    ble_conn_slot_t *slot = find_conn_by_addr(address);
    if (slot == NULL) {
        jpp_broker_error_result(result, "BLE_NOT_CONNECTED");
        return JPP_BROKER_STATUS_OK;
    }

    int rc = ble_gap_terminate(slot->handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_terminate failed: %d", rc);
    }

    memset(slot, 0, sizeof(*slot));

    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

/* ---- GATT server (claim of the pre-registered app-host service) ---------- */

static jpp_broker_status_t ble_service_register_impl(
    void *context,
    const char *svc_uuid,
    jpp_broker_result_t *result
)
{
    (void)context;
    (void)svc_uuid;
    /* The generic app-host service is pre-registered in jpp_ble_native_init
       (NimBLE requires services to be added before the host starts). This call
       just claims the host role for the app; the per-app exclusivity is enforced
       by the broker's ble_host lock and the SDK's one-registration guard. */
    if (result == NULL) return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

static jpp_broker_status_t ble_service_unregister_impl(
    void *context,
    jpp_broker_result_t *result
)
{
    (void)context;
    if (result == NULL) return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

/* ---- Generic GATT app-host service (static, pre-registered) -------------- */

/*
 * A single reusable host service shared by whichever app holds ble.host (apps
 * run serialized, so one slot is enough). It carries a readable TX
 * characteristic and a writable RX characteristic; the protocol carried over
 * them is entirely app-defined. UUIDs mirror JPP_SDK_BLE_HOST_*_UUID.
 *
 * NimBLE 128-bit UUIDs are stored little-endian in the value[] array, i.e. the
 * reverse of the textual UUID. 4a505300-0000-0000-0000-000000000000 reversed:
 *   00 00 00 00 00 00 00 00 00 00 00 00 00 53 50 4a
 */
static const ble_uuid128_t s_apphost_svc_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x50, 0x4a
);
static const ble_uuid128_t s_apphost_tx_chr_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x53, 0x50, 0x4a
);
static const ble_uuid128_t s_apphost_rx_chr_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x53, 0x50, 0x4a
);

static int apphost_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* TX char read: send s_tx_buf to the leader. */
        xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
        int rc = os_mbuf_append(ctxt->om, s_tx_buf, s_tx_len);
        xSemaphoreGive(s_tx_mutex);
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;

    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* RX char write: receive data from the leader. */
        uint16_t pkt_len = OS_MBUF_PKTLEN(ctxt->om);
        size_t copy = (pkt_len < sizeof(s_rx_buf)) ? (size_t)pkt_len : sizeof(s_rx_buf);
        xSemaphoreTake(s_rx_mutex, portMAX_DELAY);
        uint16_t rx_copied = 0u;
        ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf, (uint16_t)copy, &rx_copied);
        s_rx_len = rx_copied;
        xSemaphoreGive(s_rx_mutex);
        xSemaphoreGive(s_rx_ready);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def s_apphost_chrs[] = {
    {
        .uuid      = &s_apphost_tx_chr_uuid.u,
        .access_cb = apphost_gatt_access_cb,
        .flags     = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid      = &s_apphost_rx_chr_uuid.u,
        .access_cb = apphost_gatt_access_cb,
        .flags     = BLE_GATT_CHR_F_WRITE,
    },
    { 0 }, /* terminator */
};

static const struct ble_gatt_svc_def s_apphost_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &s_apphost_svc_uuid.u,
        .characteristics = s_apphost_chrs,
    },
    { 0 }, /* terminator */
};

/* ---- Public host buffer API ---------------------------------------------- */

void jpp_ble_host_set_tx(const uint8_t *data, size_t len)
{
    if (data == NULL) return;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    if (len > sizeof(s_tx_buf)) len = sizeof(s_tx_buf);
    memcpy(s_tx_buf, data, len);
    s_tx_len = len;
    xSemaphoreGive(s_tx_mutex);
}

bool jpp_ble_host_wait_rx(uint8_t *buf, size_t *len_out, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == 0u)
                     ? portMAX_DELAY
                     : pdMS_TO_TICKS(timeout_ms);

    if (xSemaphoreTake(s_rx_ready, ticks) != pdTRUE) {
        if (len_out) *len_out = 0u;
        return false;
    }

    xSemaphoreTake(s_rx_mutex, portMAX_DELAY);
    size_t copy = s_rx_len;
    if (buf && len_out) {
        if (copy > *len_out) copy = *len_out;
        memcpy(buf, s_rx_buf, copy);
        *len_out = copy;
    } else if (len_out) {
        *len_out = copy;
    }
    xSemaphoreGive(s_rx_mutex);
    return true;
}

void jpp_ble_host_clear_rx(void)
{
    /* Drain any pending tokens. */
    while (xSemaphoreTake(s_rx_ready, 0) == pdTRUE) {}
    xSemaphoreTake(s_rx_mutex, portMAX_DELAY);
    s_rx_len = 0u;
    xSemaphoreGive(s_rx_mutex);
}

/* ---- NimBLE host task ----------------------------------------------------- */

static void nimble_host_task(void *arg)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_hs_sync_cb(void)
{
    ESP_LOGI(TAG, "NimBLE host sync'd; svc_handle=%u", s_gatt_svc_handle);
}

static void ble_hs_reset_cb(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset: reason=%d", reason);
}

/* ---- Public API ---------------------------------------------------------- */

/* Bring the NimBLE stack up: controller + HCI + host GATT registration + host
   task.  Shared by jpp_ble_native_init() (first boot) and jpp_ble_native_resume()
   (after a suspend).  The synchronisation objects and static buffers are owned by
   jpp_ble_native_init() and persist across suspend/resume cycles, so they are NOT
   touched here. */
static void ble_stack_start(void)
{
    /* nimble_port_init() performs controller + HCI init internally; the
     * standalone esp_nimble_hci_init() call used by older ESP-IDF releases is no
     * longer needed (and would double-init the HCI). */
    nimble_port_init();

    ble_hs_cfg.sync_cb  = ble_hs_sync_cb;
    ble_hs_cfg.reset_cb = ble_hs_reset_cb;

    /* Register the generic app-host GATT service before starting the host.
       The GATT DB is reset by nimble_port_deinit(), so this must run on every
       (re-)start, not just first boot. */
    int rc = ble_gatts_count_cfg(s_apphost_svcs);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gatts_count_cfg failed: %d", rc);
    }
    rc = ble_gatts_add_svcs(s_apphost_svcs);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gatts_add_svcs failed: %d", rc);
    }

    nimble_port_freertos_init(nimble_host_task);
    s_ble_active = true;
}

void jpp_ble_native_init(void)
{
    /* Initialise synchronisation objects. */
    s_ble_done = xSemaphoreCreateBinary();
    s_tx_mutex = xSemaphoreCreateMutex();
    s_rx_mutex = xSemaphoreCreateMutex();
    s_rx_ready = xSemaphoreCreateCounting(2, 0);

    if (s_ble_done == NULL || s_tx_mutex == NULL ||
        s_rx_mutex == NULL || s_rx_ready == NULL) {
        ESP_LOGE(TAG, "BLE semaphore creation failed — out of memory");
        abort();
    }

    memset(s_conns,   0, sizeof(s_conns));
    memset(s_tx_buf,  0, sizeof(s_tx_buf));
    memset(s_rx_buf,  0, sizeof(s_rx_buf));
    s_tx_len = 0u;
    s_rx_len = 0u;

    ble_stack_start();

    ESP_LOGI(TAG, "jpp_ble_native_init complete");
}

bool jpp_ble_native_suspend(void)
{
    if (!s_ble_active) { return false; }

    /* nimble_port_stop() signals nimble_port_run() to return; the host task then
       self-deletes via nimble_port_freertos_deinit().  nimble_port_deinit()
       tears down the controller and releases its heap. */
    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "nimble_port_stop failed: %d (BLE left running)", rc);
        return false;
    }
    nimble_port_deinit();
    s_ble_active = false;

    /* Drop any stale connection state so a later resume starts clean. */
    memset(s_conns, 0, sizeof(s_conns));

    ESP_LOGW(TAG, "BLE suspended; free heap now %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return true;
}

bool jpp_ble_native_resume(void)
{
    if (s_ble_active) { return false; }
    ble_stack_start();
    ESP_LOGW(TAG, "BLE resumed; free heap now %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return true;
}

void jpp_ble_native_get_services(
    jpp_sdk_ble_scan_fn_t            *out_scan,       void **out_scan_ctx,
    jpp_sdk_ble_advertise_start_fn_t *out_adv_start,
    jpp_sdk_ble_advertise_stop_fn_t  *out_adv_stop,   void **out_adv_ctx,
    jpp_sdk_ble_connect_fn_t         *out_connect,
    jpp_sdk_ble_read_char_fn_t       *out_read_char,
    jpp_sdk_ble_write_char_fn_t      *out_write_char,
    jpp_sdk_ble_disconnect_fn_t      *out_disconnect,  void **out_client_ctx,
    jpp_sdk_ble_service_register_fn_t   *out_svc_reg,
    jpp_sdk_ble_service_unregister_fn_t *out_svc_unreg, void **out_host_ctx
)
{
    *out_scan       = ble_scan_impl;
    *out_scan_ctx   = NULL;

    *out_adv_start  = ble_advertise_start_impl;
    *out_adv_stop   = ble_advertise_stop_impl;
    *out_adv_ctx    = NULL;

    *out_connect    = ble_connect_impl;
    *out_read_char  = ble_read_char_impl;
    *out_write_char = ble_write_char_impl;
    *out_disconnect = ble_disconnect_impl;
    *out_client_ctx = NULL;

    *out_svc_reg    = ble_service_register_impl;
    *out_svc_unreg  = ble_service_unregister_impl;
    *out_host_ctx   = NULL;
}

/* Adapters matching the jpp_sdk_native_services_t host fn-pointer signatures.
   They forward to the host buffer API; the void *context is unused. */
static void ble_host_set_value_impl(void *context, const uint8_t *data, size_t len)
{
    (void)context;
    jpp_ble_host_set_tx(data, len);
}

static bool ble_host_wait_write_impl(void *context, uint8_t *buf,
                                     size_t *len_inout, uint32_t timeout_ms)
{
    (void)context;
    return jpp_ble_host_wait_rx(buf, len_inout, timeout_ms);
}

static void ble_host_clear_impl(void *context)
{
    (void)context;
    jpp_ble_host_clear_rx();
}

static void ble_set_connectable_impl(void *context, bool connectable)
{
    (void)context;
    jpp_ble_native_set_connectable(connectable);
}

void jpp_ble_native_get_host_services(
    jpp_sdk_ble_host_set_value_fn_t  *out_set_value,
    jpp_sdk_ble_host_wait_write_fn_t *out_wait_write,
    jpp_sdk_ble_host_clear_fn_t      *out_clear,
    jpp_sdk_ble_set_connectable_fn_t *out_set_connectable
)
{
    *out_set_value       = ble_host_set_value_impl;
    *out_wait_write      = ble_host_wait_write_impl;
    *out_clear           = ble_host_clear_impl;
    *out_set_connectable = ble_set_connectable_impl;
}

#else /* CONFIG_BT_ENABLED not set, or JPP_WOKWI_SIM — provide no-op stubs */

#include "esp_log.h"
static const char *TAG = "jpp_ble_native";

void jpp_ble_native_init(void)
{
    ESP_LOGW(TAG, "BT not enabled; BLE unavailable");
}

bool jpp_ble_native_suspend(void) { return false; }
bool jpp_ble_native_resume(void)  { return false; }

void jpp_ble_native_get_services(
    jpp_sdk_ble_scan_fn_t            *out_scan,       void **out_scan_ctx,
    jpp_sdk_ble_advertise_start_fn_t *out_adv_start,
    jpp_sdk_ble_advertise_stop_fn_t  *out_adv_stop,   void **out_adv_ctx,
    jpp_sdk_ble_connect_fn_t         *out_connect,
    jpp_sdk_ble_read_char_fn_t       *out_read_char,
    jpp_sdk_ble_write_char_fn_t      *out_write_char,
    jpp_sdk_ble_disconnect_fn_t      *out_disconnect,  void **out_client_ctx,
    jpp_sdk_ble_service_register_fn_t   *out_svc_reg,
    jpp_sdk_ble_service_unregister_fn_t *out_svc_unreg, void **out_host_ctx)
{
    *out_scan       = NULL; *out_scan_ctx   = NULL;
    *out_adv_start  = NULL;
    *out_adv_stop   = NULL; *out_adv_ctx    = NULL;
    *out_connect    = NULL;
    *out_read_char  = NULL;
    *out_write_char = NULL;
    *out_disconnect = NULL; *out_client_ctx = NULL;
    *out_svc_reg    = NULL;
    *out_svc_unreg  = NULL; *out_host_ctx   = NULL;
}

void jpp_ble_native_get_host_services(
    jpp_sdk_ble_host_set_value_fn_t  *out_set_value,
    jpp_sdk_ble_host_wait_write_fn_t *out_wait_write,
    jpp_sdk_ble_host_clear_fn_t      *out_clear,
    jpp_sdk_ble_set_connectable_fn_t *out_set_connectable)
{
    *out_set_value       = NULL;
    *out_wait_write      = NULL;
    *out_clear           = NULL;
    *out_set_connectable = NULL;
}

void jpp_ble_native_set_connectable(bool connectable) { (void)connectable; }

void jpp_ble_host_set_tx(const uint8_t *data, size_t len)
{
    (void)data; (void)len;
}

bool jpp_ble_host_wait_rx(uint8_t *buf, size_t *len_out, uint32_t timeout_ms)
{
    (void)buf; (void)len_out; (void)timeout_ms;
    return false;
}

void jpp_ble_host_clear_rx(void) {}

#endif /* CONFIG_BT_ENABLED && !JPP_WOKWI_SIM */
