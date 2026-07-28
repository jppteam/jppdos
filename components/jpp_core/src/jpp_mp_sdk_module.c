/*
 * jppsdk — built-in MicroPython module bridging Python apps to jpp_sdk_bridge.
 *
 * All functions operate on the context set by jpp_mp_sdk_module_set_context().
 * The context must be set before any Python code in an app session runs.
 *
 * Error mapping:
 *   JPP_SDK_STATUS_ACCESS_DENIED     → SdkPermissionError
 *   any other non-OK status          → SdkError
 * Text-truncated results are not raised as errors; the truncated text is returned.
 */

#include "../include/jpp_mp_sdk_module.h"
#include "../include/jpp_sdk_bridge.h"
#include "../include/jpp_buzzer_core.h"

/* STATIC was removed from MicroPython v1.20+; define it here for jpp_core usage. */
#define STATIC static

#include "py/obj.h"
#include "py/objexcept.h"
#include "py/runtime.h"
#include "py/builtin.h"

#include <string.h>

/* -------------------------------------------------------------------------- */
/* Context                                                                     */
/* -------------------------------------------------------------------------- */

static jpp_sdk_context_t *s_sdk_ctx;

void jpp_mp_sdk_module_set_context(jpp_sdk_context_t *ctx)
{
    s_sdk_ctx = ctx;
}

static jpp_sdk_context_t *get_ctx(void)
{
    if (s_sdk_ctx == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("jppsdk: no active app context"));
    }
    return s_sdk_ctx;
}

/* -------------------------------------------------------------------------- */
/* Exception types                                                             */
/* -------------------------------------------------------------------------- */

MP_DEFINE_EXCEPTION(SdkError, Exception)
MP_DEFINE_EXCEPTION(SdkPermissionError, SdkError)

static NORETURN void raise_sdk_error(jpp_sdk_status_t status,
                                     const jpp_broker_result_t *result)
{
    const char *msg;
    if (result && !result->ok && result->code && result->code[0] != '\0') {
        msg = result->code;
    } else {
        msg = jpp_sdk_status_name(status);
    }
    if (status == JPP_SDK_STATUS_ACCESS_DENIED) {
        mp_raise_msg(&mp_type_SdkPermissionError, MP_ERROR_TEXT(msg));
    }
    mp_raise_msg(&mp_type_SdkError, MP_ERROR_TEXT(msg));
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

/* Convert a jpp_broker_result_t to a Python dict of {key: value} strings. */
static mp_obj_t broker_result_to_dict(const jpp_broker_result_t *result)
{
    mp_obj_t dict = mp_obj_new_dict((mp_uint_t)result->field_count);
    for (size_t i = 0; i < result->field_count; i++) {
        const char *key = result->fields[i].key   ? result->fields[i].key   : "";
        const char *val = result->fields[i].value ? result->fields[i].value : "";
        mp_obj_dict_store(dict,
            mp_obj_new_str(key, strlen(key)),
            mp_obj_new_str(val, strlen(val)));
    }
    return dict;
}

/* -------------------------------------------------------------------------- */
/* Core functions                                                              */
/* -------------------------------------------------------------------------- */

/* set_frame(lines)
 * lines — list or tuple of str, up to JPP_SDK_FRAME_LINE_CAPACITY entries.
 */
STATIC mp_obj_t mp_sdk_set_frame(mp_obj_t lines_obj)
{
    jpp_sdk_context_t *ctx = get_ctx();

    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(lines_obj, &n, &items);
    if (n > JPP_SDK_FRAME_LINE_CAPACITY) {
        n = JPP_SDK_FRAME_LINE_CAPACITY;
    }

    const char *lines[JPP_SDK_FRAME_LINE_CAPACITY];
    for (size_t i = 0; i < n; i++) {
        lines[i] = mp_obj_str_get_str(items[i]);
    }

    jpp_sdk_status_t st = jpp_sdk_set_frame(ctx, lines, n);
    /* TEXT_TRUNCATED is not an error — the display clips long lines silently. */
    if (st != JPP_SDK_STATUS_OK && st != JPP_SDK_STATUS_TEXT_TRUNCATED) {
        raise_sdk_error(st, NULL);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_set_frame_obj, mp_sdk_set_frame);

/* request_close() — ask the runtime to close this app. */
STATIC mp_obj_t mp_sdk_request_close(void)
{
    jpp_sdk_status_t st = jpp_sdk_request_close(get_ctx());
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_request_close_obj, mp_sdk_request_close);

/* log(event_name) — emit a tagged log event visible in device logs. */
STATIC mp_obj_t mp_sdk_log(mp_obj_t event_obj)
{
    const char *event = mp_obj_str_get_str(event_obj);
    jpp_sdk_status_t st = jpp_sdk_log(get_ctx(), event);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_log_obj, mp_sdk_log);

/* -------------------------------------------------------------------------- */
/* Device                                                                      */
/* -------------------------------------------------------------------------- */

/* device_status() → dict  (ungated) */
STATIC mp_obj_t mp_sdk_device_status(void)
{
    jpp_broker_result_t result;
    jpp_sdk_status_t st = jpp_sdk_device_status(get_ctx(), &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return broker_result_to_dict(&result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_device_status_obj, mp_sdk_device_status);

/* get_time() → str "YYYY-MM-DD HH:mm"  (ungated) */
STATIC mp_obj_t mp_sdk_get_time(void)
{
    jpp_broker_result_t result;
    jpp_sdk_status_t st = jpp_sdk_get_time(get_ctx(), &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    const char *text = jpp_broker_result_get(&result, "text");
    if (text == NULL) text = "";
    return mp_obj_new_str(text, strlen(text));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_get_time_obj, mp_sdk_get_time);

/* is_dummy_mode() → bool  (ungated) */
STATIC mp_obj_t mp_sdk_is_dummy_mode(void)
{
    return mp_obj_new_bool(jpp_sdk_is_dummy_mode(get_ctx()));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_is_dummy_mode_obj, mp_sdk_is_dummy_mode);

/* -------------------------------------------------------------------------- */
/* Scoped file I/O  (ungated — sandboxed to /sd/apps/<app_id>/)              */
/* -------------------------------------------------------------------------- */

/* file_read(path) → dict */
STATIC mp_obj_t mp_sdk_file_read(mp_obj_t path_obj)
{
    jpp_broker_result_t result;
    const char *path = mp_obj_str_get_str(path_obj);
    jpp_sdk_status_t st = jpp_sdk_file_read(get_ctx(), path, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return broker_result_to_dict(&result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_file_read_obj, mp_sdk_file_read);

/* file_write(path, text) */
STATIC mp_obj_t mp_sdk_file_write(mp_obj_t path_obj, mp_obj_t text_obj)
{
    jpp_broker_result_t result;
    const char *path = mp_obj_str_get_str(path_obj);
    const char *text = mp_obj_str_get_str(text_obj);
    jpp_sdk_status_t st = jpp_sdk_file_write(get_ctx(), path, text, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mp_sdk_file_write_obj, mp_sdk_file_write);

/* file_list(dir) → dict */
STATIC mp_obj_t mp_sdk_file_list(mp_obj_t dir_obj)
{
    jpp_broker_result_t result;
    const char *dir = mp_obj_str_get_str(dir_obj);
    jpp_sdk_status_t st = jpp_sdk_file_list(get_ctx(), dir, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return broker_result_to_dict(&result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_file_list_obj, mp_sdk_file_list);

/* -------------------------------------------------------------------------- */
/* Shared file I/O  (ungated — sandboxed to /sd/shared/<app_id>/)            */
/* -------------------------------------------------------------------------- */

/* shared_read(path) → dict */
STATIC mp_obj_t mp_sdk_shared_read(mp_obj_t path_obj)
{
    jpp_broker_result_t result;
    const char *path = mp_obj_str_get_str(path_obj);
    jpp_sdk_status_t st = jpp_sdk_shared_read(get_ctx(), path, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return broker_result_to_dict(&result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_shared_read_obj, mp_sdk_shared_read);

/* shared_write(path, text) */
STATIC mp_obj_t mp_sdk_shared_write(mp_obj_t path_obj, mp_obj_t text_obj)
{
    jpp_broker_result_t result;
    const char *path = mp_obj_str_get_str(path_obj);
    const char *text = mp_obj_str_get_str(text_obj);
    jpp_sdk_status_t st = jpp_sdk_shared_write(get_ctx(), path, text, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mp_sdk_shared_write_obj, mp_sdk_shared_write);

/* shared_list(dir) → dict */
STATIC mp_obj_t mp_sdk_shared_list(mp_obj_t dir_obj)
{
    jpp_broker_result_t result;
    const char *dir = mp_obj_str_get_str(dir_obj);
    jpp_sdk_status_t st = jpp_sdk_shared_list(get_ctx(), dir, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return broker_result_to_dict(&result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_shared_list_obj, mp_sdk_shared_list);

/* -------------------------------------------------------------------------- */
/* Full file I/O via handles  (requires: files.full)                          */
/* -------------------------------------------------------------------------- */

/* file_open(path, mode) → int handle
 * mode: 0 = read, 1 = write  (jppsdk.OPEN_READ / jppsdk.OPEN_WRITE)
 */
STATIC mp_obj_t mp_sdk_file_open(mp_obj_t path_obj, mp_obj_t mode_obj)
{
    const char *path = mp_obj_str_get_str(path_obj);
    jpp_sdk_open_mode_t mode = (jpp_sdk_open_mode_t)mp_obj_get_int(mode_obj);
    jpp_sdk_handle_t handle = JPP_SDK_HANDLE_INVALID;
    jpp_sdk_status_t st = jpp_sdk_file_open(get_ctx(), path, mode, &handle);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_obj_new_int(handle);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mp_sdk_file_open_obj, mp_sdk_file_open);

/* handle_read(handle) → dict */
STATIC mp_obj_t mp_sdk_handle_read(mp_obj_t handle_obj)
{
    jpp_broker_result_t result;
    jpp_sdk_handle_t handle = (jpp_sdk_handle_t)mp_obj_get_int(handle_obj);
    jpp_sdk_status_t st = jpp_sdk_handle_read(get_ctx(), handle, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return broker_result_to_dict(&result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_handle_read_obj, mp_sdk_handle_read);

/* handle_write(handle, text) */
STATIC mp_obj_t mp_sdk_handle_write(mp_obj_t handle_obj, mp_obj_t text_obj)
{
    jpp_broker_result_t result;
    jpp_sdk_handle_t handle = (jpp_sdk_handle_t)mp_obj_get_int(handle_obj);
    const char *text = mp_obj_str_get_str(text_obj);
    jpp_sdk_status_t st = jpp_sdk_handle_write(get_ctx(), handle, text, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mp_sdk_handle_write_obj, mp_sdk_handle_write);

/* handle_list(handle) → dict */
STATIC mp_obj_t mp_sdk_handle_list(mp_obj_t handle_obj)
{
    jpp_broker_result_t result;
    jpp_sdk_handle_t handle = (jpp_sdk_handle_t)mp_obj_get_int(handle_obj);
    jpp_sdk_status_t st = jpp_sdk_handle_list(get_ctx(), handle, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return broker_result_to_dict(&result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_handle_list_obj, mp_sdk_handle_list);

/* handle_close(handle) */
STATIC mp_obj_t mp_sdk_handle_close(mp_obj_t handle_obj)
{
    jpp_sdk_handle_t handle = (jpp_sdk_handle_t)mp_obj_get_int(handle_obj);
    jpp_sdk_status_t st = jpp_sdk_handle_close(get_ctx(), handle);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_handle_close_obj, mp_sdk_handle_close);

/* -------------------------------------------------------------------------- */
/* Input                                                                       */
/* -------------------------------------------------------------------------- */

/* poll_key() → int  (KEY_NONE = 0 if queue is empty) */
STATIC mp_obj_t mp_sdk_poll_key(void)
{
    jpp_sdk_key_event_t event = JPP_SDK_KEY_NONE;
    jpp_sdk_status_t st = jpp_sdk_poll_key(get_ctx(), &event);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_obj_new_int(event);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_poll_key_obj, mp_sdk_poll_key);

/* wait_key(timeout_ms) → int */
STATIC mp_obj_t mp_sdk_wait_key(mp_obj_t timeout_obj)
{
    uint32_t timeout_ms = (uint32_t)mp_obj_get_int(timeout_obj);
    jpp_sdk_key_event_t event = JPP_SDK_KEY_NONE;
    jpp_sdk_status_t st = jpp_sdk_wait_key(get_ctx(), timeout_ms, &event);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_obj_new_int(event);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_wait_key_obj, mp_sdk_wait_key);

/* claim_center(mask) → None */
STATIC mp_obj_t mp_sdk_claim_center(mp_obj_t mask_obj)
{
    mp_int_t mask = mp_obj_get_int(mask_obj);
    if (mask < 0 || mask > 0xFF) {
        raise_sdk_error(JPP_SDK_STATUS_INVALID_ARGUMENT, NULL);
    }
    jpp_sdk_status_t st = jpp_sdk_claim_center(get_ctx(), (uint8_t)mask);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_claim_center_obj, mp_sdk_claim_center);

/* -------------------------------------------------------------------------- */
/* BLE — scan  (requires: ble.scan)                                           */
/* -------------------------------------------------------------------------- */

/* ble_scan(duration_ms) → list of dicts
 * Each dict: {"address": str, "name": str, "rssi": int, "ad_data": bytes}
 */
STATIC mp_obj_t mp_sdk_ble_scan(mp_obj_t duration_obj)
{
    jpp_broker_result_t result;
    jpp_sdk_ble_scan_result_t results[JPP_SDK_BLE_SCAN_MAX];
    size_t out_count = 0;
    uint32_t duration_ms = (uint32_t)mp_obj_get_int(duration_obj);

    jpp_sdk_status_t st = jpp_sdk_ble_scan(
        get_ctx(), duration_ms, results, JPP_SDK_BLE_SCAN_MAX, &out_count, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }

    mp_obj_t list = mp_obj_new_list((mp_uint_t)out_count, NULL);
    for (size_t i = 0; i < out_count; i++) {
        mp_obj_t entry = mp_obj_new_dict(4);
        mp_obj_dict_store(entry,
            MP_OBJ_NEW_QSTR(MP_QSTR_address),
            mp_obj_new_str(results[i].address, strlen(results[i].address)));
        mp_obj_dict_store(entry,
            MP_OBJ_NEW_QSTR(MP_QSTR_name),
            mp_obj_new_str(results[i].name, strlen(results[i].name)));
        mp_obj_dict_store(entry,
            MP_OBJ_NEW_QSTR(MP_QSTR_rssi),
            mp_obj_new_int(results[i].rssi));
        mp_obj_dict_store(entry,
            MP_OBJ_NEW_QSTR(MP_QSTR_ad_data),
            mp_obj_new_bytes(results[i].ad_data, results[i].ad_data_len));
        mp_obj_list_store(list, mp_obj_new_int((mp_int_t)i), entry);
    }
    return list;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_ble_scan_obj, mp_sdk_ble_scan);

/* -------------------------------------------------------------------------- */
/* BLE — advertise  (requires: ble.advertise)                                 */
/* -------------------------------------------------------------------------- */

/* ble_advertise_start(payload_bytes) */
STATIC mp_obj_t mp_sdk_ble_advertise_start(mp_obj_t payload_obj)
{
    jpp_broker_result_t result;
    mp_buffer_info_t buf;
    mp_get_buffer_raise(payload_obj, &buf, MP_BUFFER_READ);
    jpp_sdk_status_t st = jpp_sdk_ble_advertise_start(
        get_ctx(), (const uint8_t *)buf.buf, buf.len, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_ble_advertise_start_obj, mp_sdk_ble_advertise_start);

/* ble_advertise_stop() */
STATIC mp_obj_t mp_sdk_ble_advertise_stop(void)
{
    jpp_broker_result_t result;
    jpp_sdk_status_t st = jpp_sdk_ble_advertise_stop(get_ctx(), &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_ble_advertise_stop_obj, mp_sdk_ble_advertise_stop);

/* -------------------------------------------------------------------------- */
/* ESP-NOW  (requires: esp_now)                                               */
/* -------------------------------------------------------------------------- */

/* espnow_send(peer_mac: bytes[6], data: bytes) */
STATIC mp_obj_t mp_sdk_espnow_send(mp_obj_t mac_obj, mp_obj_t data_obj)
{
    jpp_broker_result_t result;
    mp_buffer_info_t mac_buf, data_buf;
    mp_get_buffer_raise(mac_obj, &mac_buf, MP_BUFFER_READ);
    mp_get_buffer_raise(data_obj, &data_buf, MP_BUFFER_READ);
    if (mac_buf.len != 6u) {
        mp_raise_ValueError(MP_ERROR_TEXT("peer_mac must be 6 bytes"));
    }
    jpp_sdk_status_t st = jpp_sdk_espnow_send(
        get_ctx(), (const uint8_t *)mac_buf.buf, (const uint8_t *)data_buf.buf,
        data_buf.len, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mp_sdk_espnow_send_obj, mp_sdk_espnow_send);

/* espnow_recv(timeout_ms: int) -> (peer_mac_bytes, data_bytes) or None on timeout */
STATIC mp_obj_t mp_sdk_espnow_recv(mp_obj_t timeout_obj)
{
    jpp_broker_result_t result;
    uint8_t peer_mac[6];
    uint8_t data[JPP_SDK_ESPNOW_DATA_MAX];
    size_t out_len = 0u;
    uint32_t timeout_ms = (uint32_t)mp_obj_get_int(timeout_obj);

    jpp_sdk_status_t st = jpp_sdk_espnow_recv(
        get_ctx(), peer_mac, data, sizeof(data), &out_len, timeout_ms, &result);
    if (st == JPP_SDK_STATUS_NO_DATA) {
        return mp_const_none;
    }
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    mp_obj_t tuple[2] = {
        mp_obj_new_bytes(peer_mac, sizeof(peer_mac)),
        mp_obj_new_bytes(data, out_len),
    };
    return mp_obj_new_tuple(2, tuple);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_espnow_recv_obj, mp_sdk_espnow_recv);

/* -------------------------------------------------------------------------- */
/* BLE — GATT client  (requires: ble.connect)                                 */
/* -------------------------------------------------------------------------- */

/* ble_connect(address) → int conn_handle */
STATIC mp_obj_t mp_sdk_ble_connect(mp_obj_t addr_obj)
{
    const char *address = mp_obj_str_get_str(addr_obj);
    jpp_sdk_ble_conn_t conn = JPP_SDK_BLE_CONN_INVALID;
    jpp_sdk_status_t st = jpp_sdk_ble_connect(get_ctx(), address, &conn);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_obj_new_int(conn);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_ble_connect_obj, mp_sdk_ble_connect);

/* ble_read_char(conn, svc_uuid, char_uuid) → dict */
STATIC mp_obj_t mp_sdk_ble_read_char(mp_obj_t conn_obj, mp_obj_t svc_obj, mp_obj_t char_obj)
{
    jpp_broker_result_t result;
    jpp_sdk_ble_conn_t conn = (jpp_sdk_ble_conn_t)mp_obj_get_int(conn_obj);
    const char *svc_uuid  = mp_obj_str_get_str(svc_obj);
    const char *char_uuid = mp_obj_str_get_str(char_obj);
    jpp_sdk_status_t st = jpp_sdk_ble_read_char(get_ctx(), conn, svc_uuid, char_uuid, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return broker_result_to_dict(&result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(mp_sdk_ble_read_char_obj, mp_sdk_ble_read_char);

/* ble_write_char(conn, svc_uuid, char_uuid, text) */
STATIC mp_obj_t mp_sdk_ble_write_char(size_t n_args, const mp_obj_t *args)
{
    jpp_broker_result_t result;
    jpp_sdk_ble_conn_t conn = (jpp_sdk_ble_conn_t)mp_obj_get_int(args[0]);
    const char *svc_uuid  = mp_obj_str_get_str(args[1]);
    const char *char_uuid = mp_obj_str_get_str(args[2]);
    const char *text      = mp_obj_str_get_str(args[3]);
    jpp_sdk_status_t st = jpp_sdk_ble_write_char(
        get_ctx(), conn, svc_uuid, char_uuid, text, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
    (void)n_args;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_sdk_ble_write_char_obj, 4, 4, mp_sdk_ble_write_char);

/* ble_disconnect(conn) */
STATIC mp_obj_t mp_sdk_ble_disconnect(mp_obj_t conn_obj)
{
    jpp_sdk_ble_conn_t conn = (jpp_sdk_ble_conn_t)mp_obj_get_int(conn_obj);
    jpp_sdk_status_t st = jpp_sdk_ble_disconnect(get_ctx(), conn);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_ble_disconnect_obj, mp_sdk_ble_disconnect);

/* -------------------------------------------------------------------------- */
/* BLE — GATT server  (requires: ble.host)                                    */
/* -------------------------------------------------------------------------- */

/* ble_service_register(svc_uuid) */
STATIC mp_obj_t mp_sdk_ble_service_register(mp_obj_t svc_obj)
{
    jpp_broker_result_t result;
    const char *svc_uuid = mp_obj_str_get_str(svc_obj);
    jpp_sdk_status_t st = jpp_sdk_ble_service_register(get_ctx(), svc_uuid, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_ble_service_register_obj, mp_sdk_ble_service_register);

/* ble_service_unregister() */
STATIC mp_obj_t mp_sdk_ble_service_unregister(void)
{
    jpp_broker_result_t result;
    jpp_sdk_status_t st = jpp_sdk_ble_service_unregister(get_ctx(), &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_ble_service_unregister_obj, mp_sdk_ble_service_unregister);

/* -------------------------------------------------------------------------- */
/* Background tasks  (requires: background.register)                          */
/* -------------------------------------------------------------------------- */

/* background_register() → None — consent trigger; the schedule itself comes
   from the manifest's background.tasks and is synced at app exit. */
STATIC mp_obj_t mp_sdk_background_register(void)
{
    jpp_broker_result_t result;
    jpp_sdk_status_t st = jpp_sdk_background_register(get_ctx(), &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_background_register_obj, mp_sdk_background_register);

/* -------------------------------------------------------------------------- */
/* HTTP  (requires: http.request)                                             */
/* -------------------------------------------------------------------------- */

/* http_request(method, url) → dict  or  http_request(method, url, body) → dict */
STATIC mp_obj_t mp_sdk_http_request(size_t n_args, const mp_obj_t *args)
{
    jpp_broker_result_t result;
    const char *method = mp_obj_str_get_str(args[0]);
    const char *url    = mp_obj_str_get_str(args[1]);
    const char *body   = (n_args >= 3 && args[2] != mp_const_none)
                         ? mp_obj_str_get_str(args[2]) : NULL;
    jpp_sdk_status_t st = jpp_sdk_http_request(get_ctx(), method, url, body, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return broker_result_to_dict(&result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_sdk_http_request_obj, 2, 3, mp_sdk_http_request);

/* https_request(method, url) → dict  or  https_request(method, url, body) → dict
   Requires https.request; the URL must be https:// and the first request to a
   given origin raises a per-origin consent prompt. */
STATIC mp_obj_t mp_sdk_https_request(size_t n_args, const mp_obj_t *args)
{
    jpp_broker_result_t result;
    const char *method = mp_obj_str_get_str(args[0]);
    const char *url    = mp_obj_str_get_str(args[1]);
    const char *body   = (n_args >= 3 && args[2] != mp_const_none)
                         ? mp_obj_str_get_str(args[2]) : NULL;
    jpp_sdk_status_t st = jpp_sdk_https_request(get_ctx(), method, url, body, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return broker_result_to_dict(&result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_sdk_https_request_obj, 2, 3, mp_sdk_https_request);

/* -------------------------------------------------------------------------- */
/* Network  (requires: network.bind)                                          */
/* -------------------------------------------------------------------------- */

/* net_bind(port) → None */
STATIC mp_obj_t mp_sdk_net_bind(mp_obj_t port_obj)
{
    jpp_broker_result_t result;
    mp_int_t port = mp_obj_get_int(port_obj);
    if (port <= 0 || port > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("port must be 1-65535"));
    }
    jpp_sdk_status_t st = jpp_sdk_net_bind(get_ctx(), (uint16_t)port, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_net_bind_obj, mp_sdk_net_bind);

/* net_accept(timeout_ms) → int socket id, or None on timeout */
STATIC mp_obj_t mp_sdk_net_accept(mp_obj_t timeout_obj)
{
    jpp_broker_result_t result;
    int sock = -1;
    uint32_t timeout_ms = (uint32_t)mp_obj_get_int(timeout_obj);
    jpp_sdk_status_t st = jpp_sdk_net_accept(get_ctx(), timeout_ms, &sock, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    if (sock < 0) {
        return mp_const_none;
    }
    return mp_obj_new_int(sock);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_net_accept_obj, mp_sdk_net_accept);

/* net_recv(sock, max_len, timeout_ms) → bytes (b"" on timeout or peer close) */
STATIC mp_obj_t mp_sdk_net_recv(mp_obj_t sock_obj, mp_obj_t max_obj, mp_obj_t timeout_obj)
{
    jpp_broker_result_t result;
    int sock        = (int)mp_obj_get_int(sock_obj);
    mp_int_t maxlen = mp_obj_get_int(max_obj);
    uint32_t timeout_ms = (uint32_t)mp_obj_get_int(timeout_obj);
    static uint8_t s_net_rx[1024];
    if (maxlen <= 0 || (size_t)maxlen > sizeof(s_net_rx)) {
        maxlen = (mp_int_t)sizeof(s_net_rx);
    }
    size_t out_len = 0u;
    jpp_sdk_status_t st = jpp_sdk_net_recv(get_ctx(), sock, s_net_rx,
                                           (size_t)maxlen, &out_len,
                                           timeout_ms, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_obj_new_bytes(s_net_rx, out_len);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(mp_sdk_net_recv_obj, mp_sdk_net_recv);

/* net_send(sock, data) → None */
STATIC mp_obj_t mp_sdk_net_send(mp_obj_t sock_obj, mp_obj_t data_obj)
{
    jpp_broker_result_t result;
    int sock = (int)mp_obj_get_int(sock_obj);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_obj, &buf, MP_BUFFER_READ);
    jpp_sdk_status_t st = jpp_sdk_net_send(get_ctx(), sock,
                                           (const uint8_t *)buf.buf, buf.len,
                                           &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mp_sdk_net_send_obj, mp_sdk_net_send);

/* net_close(sock) → None  (sock = -1 closes the listener) */
STATIC mp_obj_t mp_sdk_net_close(mp_obj_t sock_obj)
{
    jpp_broker_result_t result;
    int sock = (int)mp_obj_get_int(sock_obj);
    jpp_sdk_status_t st = jpp_sdk_net_close(get_ctx(), sock, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_net_close_obj, mp_sdk_net_close);

/* -------------------------------------------------------------------------- */
/* Canvas  (no capability required)                                           */
/* -------------------------------------------------------------------------- */

/* canvas_write(row, pixels) — pixels must be bytes of length 16 */
STATIC mp_obj_t mp_sdk_canvas_write(mp_obj_t row_obj, mp_obj_t pixels_obj)
{
    uint8_t row = (uint8_t)mp_obj_get_int(row_obj);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(pixels_obj, &buf, MP_BUFFER_READ);
    if (buf.len < JPP_SDK_CANVAS_ROW_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("pixels must be 16 bytes"));
    }
    jpp_sdk_status_t st = jpp_sdk_canvas_write(get_ctx(), row, (const uint8_t *)buf.buf);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mp_sdk_canvas_write_obj, mp_sdk_canvas_write);

/* canvas_clear() */
STATIC mp_obj_t mp_sdk_canvas_clear(void)
{
    jpp_sdk_status_t st = jpp_sdk_canvas_clear(get_ctx());
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_canvas_clear_obj, mp_sdk_canvas_clear);

/* canvas_draw_pixel(x, y, on) */
STATIC mp_obj_t mp_sdk_canvas_draw_pixel(mp_obj_t x_obj, mp_obj_t y_obj, mp_obj_t on_obj)
{
    uint8_t x  = (uint8_t)mp_obj_get_int(x_obj);
    uint8_t y  = (uint8_t)mp_obj_get_int(y_obj);
    bool    on = mp_obj_is_true(on_obj);
    jpp_sdk_status_t st = jpp_sdk_canvas_draw_pixel(get_ctx(), x, y, on);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(mp_sdk_canvas_draw_pixel_obj, mp_sdk_canvas_draw_pixel);

/* canvas_fullscreen(on) — toggle the 128×64 fullscreen canvas mode */
STATIC mp_obj_t mp_sdk_canvas_fullscreen(mp_obj_t on_obj)
{
    bool on = mp_obj_is_true(on_obj);
    jpp_sdk_status_t st = jpp_sdk_canvas_fullscreen(get_ctx(), on);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_canvas_fullscreen_obj, mp_sdk_canvas_fullscreen);

/* -------------------------------------------------------------------------- */
/* IPC  (ungated — mailbox files live in the recipient's scoped storage)     */
/* -------------------------------------------------------------------------- */

/* ipc_send(recipient_id, payload) → None */
STATIC mp_obj_t mp_sdk_ipc_send(mp_obj_t recipient_obj, mp_obj_t payload_obj)
{
    jpp_broker_result_t result;
    const char *recipient = mp_obj_str_get_str(recipient_obj);
    const char *payload   = mp_obj_str_get_str(payload_obj);
    jpp_sdk_status_t st = jpp_sdk_ipc_send(get_ctx(), recipient, payload, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mp_sdk_ipc_send_obj, mp_sdk_ipc_send);

/* ipc_recv() → (payload_str, sender_str) or None if no messages */
STATIC mp_obj_t mp_sdk_ipc_recv(void)
{
    jpp_broker_result_t result;
    char payload[JPP_SDK_IPC_PAYLOAD_MAX];
    payload[0] = '\0';
    jpp_sdk_status_t st = jpp_sdk_ipc_recv(get_ctx(), payload, sizeof(payload), &result);
    if (st == JPP_SDK_STATUS_NO_DATA) {
        return mp_const_none;   /* empty mailbox */
    }
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        raise_sdk_error(st, &result);
    }
    const char *sender = jpp_broker_result_get(&result, "sender");
    if (sender == NULL) sender = "";
    mp_obj_t tuple[2] = {
        mp_obj_new_str(payload, strlen(payload)),
        mp_obj_new_str(sender, strlen(sender)),
    };
    return mp_obj_new_tuple(2, tuple);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_ipc_recv_obj, mp_sdk_ipc_recv);

/* -------------------------------------------------------------------------- */
/* KV helper  (ungated — JSON file in the app's scoped storage)              */
/* -------------------------------------------------------------------------- */

/* kv_get(key) → str or None */
STATIC mp_obj_t mp_sdk_kv_get(mp_obj_t key_obj)
{
    const char *key = mp_obj_str_get_str(key_obj);
    char value[JPP_SDK_KV_VALUE_MAX];
    value[0] = '\0';
    jpp_sdk_status_t st = jpp_sdk_kv_get(get_ctx(), key, value, sizeof(value));
    if (st == JPP_SDK_STATUS_ACCESS_DENIED) {
        raise_sdk_error(st, NULL);
    }
    if (st != JPP_SDK_STATUS_OK) {
        /* Key not found — return None */
        return mp_const_none;
    }
    return mp_obj_new_str(value, strlen(value));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_kv_get_obj, mp_sdk_kv_get);

/* kv_set(key, value) → None */
STATIC mp_obj_t mp_sdk_kv_set(mp_obj_t key_obj, mp_obj_t value_obj)
{
    const char *key   = mp_obj_str_get_str(key_obj);
    const char *value = mp_obj_str_get_str(value_obj);
    jpp_sdk_status_t st = jpp_sdk_kv_set(get_ctx(), key, value);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mp_sdk_kv_set_obj, mp_sdk_kv_set);

/* kv_delete(key) → None */
STATIC mp_obj_t mp_sdk_kv_delete(mp_obj_t key_obj)
{
    const char *key = mp_obj_str_get_str(key_obj);
    jpp_sdk_status_t st = jpp_sdk_kv_delete(get_ctx(), key);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_kv_delete_obj, mp_sdk_kv_delete);

/* -------------------------------------------------------------------------- */
/* High-level UI: Dialog, List, Input                                          */
/* -------------------------------------------------------------------------- */

/* dialog(text, title=None) -> bool  (True if OK, False if BACK) */
STATIC mp_obj_t mp_sdk_dialog(size_t n_args, const mp_obj_t *args)
{
    const char *text  = mp_obj_str_get_str(args[0]);
    const char *title = (n_args >= 2 && args[1] != mp_const_none)
                        ? mp_obj_str_get_str(args[1]) : NULL;
    jpp_sdk_ui_result_t res = JPP_SDK_UI_BACK;
    jpp_sdk_status_t st = jpp_sdk_dialog(get_ctx(), title, text, &res);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    return mp_obj_new_bool(res == JPP_SDK_UI_OK);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_sdk_dialog_obj, 1, 2, mp_sdk_dialog);

/* list(items, title=None, multiselect=False) -> int | list[int] | None */
STATIC mp_obj_t mp_sdk_list(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_items, ARG_title, ARG_multiselect };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_items,       MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_title,       MP_ARG_OBJ,                   {.u_obj = mp_const_none} },
        { MP_QSTR_multiselect, MP_ARG_BOOL,                  {.u_bool = false} },
    };
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, vals);

    size_t count;
    mp_obj_t *items_obj;
    mp_obj_get_array(vals[ARG_items].u_obj, &count, &items_obj);
    if (count == 0u || count > JPP_SDK_LIST_ITEM_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("list: expected 1..16 items"));
    }
    const char *items[JPP_SDK_LIST_ITEM_MAX];
    for (size_t i = 0u; i < count; i++) {
        items[i] = mp_obj_str_get_str(items_obj[i]);
    }
    const char *title = (vals[ARG_title].u_obj != mp_const_none)
                        ? mp_obj_str_get_str(vals[ARG_title].u_obj) : NULL;
    bool multi = vals[ARG_multiselect].u_bool;

    size_t out_idx[JPP_SDK_LIST_ITEM_MAX];
    size_t out_count = 0u;
    jpp_sdk_ui_result_t res = JPP_SDK_UI_BACK;
    jpp_sdk_status_t st = jpp_sdk_list(get_ctx(), title, items, count, multi,
                                       out_idx, JPP_SDK_LIST_ITEM_MAX, &out_count, &res);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    if (res == JPP_SDK_UI_BACK) {
        return mp_const_none;
    }
    if (!multi) {
        return mp_obj_new_int((mp_int_t)out_idx[0]);
    }
    mp_obj_t lst = mp_obj_new_list((mp_uint_t)out_count, NULL);
    for (size_t i = 0u; i < out_count; i++) {
        mp_obj_list_store(lst, mp_obj_new_int((mp_int_t)i),
                          mp_obj_new_int((mp_int_t)out_idx[i]));
    }
    return lst;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mp_sdk_list_obj, 1, mp_sdk_list);

/* input(title, placeholder=None, type=INPUT_TEXT) -> str | None */
STATIC mp_obj_t mp_sdk_input(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_title, ARG_placeholder, ARG_type };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_title,       MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_placeholder, MP_ARG_OBJ,                   {.u_obj = mp_const_none} },
        { MP_QSTR_type,        MP_ARG_INT,                   {.u_int = JPP_SDK_INPUT_TEXT} },
    };
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, vals);

    const char *title = mp_obj_str_get_str(vals[ARG_title].u_obj);
    const char *placeholder = (vals[ARG_placeholder].u_obj != mp_const_none)
                              ? mp_obj_str_get_str(vals[ARG_placeholder].u_obj) : NULL;
    jpp_sdk_input_type_t type = (jpp_sdk_input_type_t)vals[ARG_type].u_int;

    char value[JPP_SDK_INPUT_VALUE_MAX + 1u];
    value[0] = '\0';
    jpp_sdk_ui_result_t res = JPP_SDK_UI_BACK;
    jpp_sdk_status_t st = jpp_sdk_input(get_ctx(), title, placeholder, type,
                                        value, sizeof(value), &res);
    if (st != JPP_SDK_STATUS_OK) {
        raise_sdk_error(st, NULL);
    }
    if (res == JPP_SDK_UI_BACK) {
        return mp_const_none;
    }
    return mp_obj_new_str(value, strlen(value));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mp_sdk_input_obj, 1, mp_sdk_input);

/* -------------------------------------------------------------------------- */
/* Wakelock                                                                    */
/* -------------------------------------------------------------------------- */

/* wakelock_acquire() */
STATIC mp_obj_t mp_sdk_wakelock_acquire(void)
{
    jpp_sdk_status_t st = jpp_sdk_wakelock_acquire(get_ctx());
    if (st != JPP_SDK_STATUS_OK) { raise_sdk_error(st, NULL); }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_wakelock_acquire_obj, mp_sdk_wakelock_acquire);

/* wakelock_release() */
STATIC mp_obj_t mp_sdk_wakelock_release(void)
{
    jpp_sdk_status_t st = jpp_sdk_wakelock_release(get_ctx());
    if (st != JPP_SDK_STATUS_OK) { raise_sdk_error(st, NULL); }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_wakelock_release_obj, mp_sdk_wakelock_release);

/* -------------------------------------------------------------------------- */
/* Buzzer                                                                      */
/* -------------------------------------------------------------------------- */

/* buzzer_play(sound_int) */
STATIC mp_obj_t mp_sdk_buzzer_play(mp_obj_t sound_obj)
{
    jpp_buzzer_sound_t sound = (jpp_buzzer_sound_t)mp_obj_get_int(sound_obj);
    jpp_sdk_status_t st = jpp_sdk_buzzer_play(get_ctx(), sound);
    if (st != JPP_SDK_STATUS_OK) { raise_sdk_error(st, NULL); }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_buzzer_play_obj, mp_sdk_buzzer_play);

/* buzzer_tone(freq_hz, duration_ms) */
STATIC mp_obj_t mp_sdk_buzzer_tone(mp_obj_t freq_obj, mp_obj_t dur_obj)
{
    uint32_t freq = (uint32_t)mp_obj_get_int(freq_obj);
    uint32_t dur  = (uint32_t)mp_obj_get_int(dur_obj);
    jpp_sdk_status_t st = jpp_sdk_buzzer_tone(get_ctx(), freq, dur);
    if (st != JPP_SDK_STATUS_OK) { raise_sdk_error(st, NULL); }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mp_sdk_buzzer_tone_obj, mp_sdk_buzzer_tone);

/* Parse a list of (freq_hz, duration_ms) pairs into notes[]; returns count. */
STATIC size_t mp_sdk_parse_notes(mp_obj_t seq_obj, jpp_buzzer_note_t *notes, size_t max)
{
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(seq_obj, &n, &items);
    if (n > max) { n = max; }
    for (size_t i = 0u; i < n; i++) {
        size_t pair_len;
        mp_obj_t *pair;
        mp_obj_get_array(items[i], &pair_len, &pair);
        if (pair_len < 2u) {
            mp_raise_ValueError(MP_ERROR_TEXT("each note must be (freq_hz, duration_ms)"));
        }
        notes[i].freq_hz     = (uint32_t)mp_obj_get_int(pair[0]);
        notes[i].duration_ms = (uint32_t)mp_obj_get_int(pair[1]);
    }
    return n;
}

/* buzzer_play_sequence(list_of_(freq,dur)) */
STATIC mp_obj_t mp_sdk_buzzer_play_sequence(mp_obj_t seq_obj)
{
    jpp_buzzer_note_t notes[64];
    size_t n = mp_sdk_parse_notes(seq_obj, notes, 64u);
    if (n == 0u) { return mp_const_none; }
    jpp_sdk_status_t st = jpp_sdk_buzzer_play_sequence(get_ctx(), notes, n);
    if (st != JPP_SDK_STATUS_OK) { raise_sdk_error(st, NULL); }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_buzzer_play_sequence_obj, mp_sdk_buzzer_play_sequence);

/* buzzer_play_sequence_async(list_of_(freq,dur)) — returns immediately */
STATIC mp_obj_t mp_sdk_buzzer_play_sequence_async(mp_obj_t seq_obj)
{
    jpp_buzzer_note_t notes[64];
    size_t n = mp_sdk_parse_notes(seq_obj, notes, 64u);
    if (n == 0u) { return mp_const_none; }
    jpp_sdk_status_t st = jpp_sdk_buzzer_play_sequence_async(get_ctx(), notes, n);
    if (st != JPP_SDK_STATUS_OK) { raise_sdk_error(st, NULL); }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mp_sdk_buzzer_play_sequence_async_obj, mp_sdk_buzzer_play_sequence_async);

/* buzzer_stop() */
STATIC mp_obj_t mp_sdk_buzzer_stop(void)
{
    jpp_sdk_status_t st = jpp_sdk_buzzer_stop(get_ctx());
    if (st != JPP_SDK_STATUS_OK) { raise_sdk_error(st, NULL); }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_buzzer_stop_obj, mp_sdk_buzzer_stop);

/* -------------------------------------------------------------------------- */
/* LED                                                                         */
/* -------------------------------------------------------------------------- */

/* led_set_color(r, g, b) */
STATIC mp_obj_t mp_sdk_led_set_color(mp_obj_t r_obj, mp_obj_t g_obj, mp_obj_t b_obj)
{
    uint8_t r = (uint8_t)mp_obj_get_int(r_obj);
    uint8_t g = (uint8_t)mp_obj_get_int(g_obj);
    uint8_t b = (uint8_t)mp_obj_get_int(b_obj);
    jpp_sdk_status_t st = jpp_sdk_led_set_color(get_ctx(), r, g, b);
    if (st != JPP_SDK_STATUS_OK) { raise_sdk_error(st, NULL); }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(mp_sdk_led_set_color_obj, mp_sdk_led_set_color);

/* led_off() */
STATIC mp_obj_t mp_sdk_led_off(void)
{
    jpp_sdk_status_t st = jpp_sdk_led_off(get_ctx());
    if (st != JPP_SDK_STATUS_OK) { raise_sdk_error(st, NULL); }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_sdk_led_off_obj, mp_sdk_led_off);

/* -------------------------------------------------------------------------- */
/* Module definition                                                           */
/* -------------------------------------------------------------------------- */

STATIC const mp_rom_map_elem_t jppsdk_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_jppsdk) },

    /* Exception types */
    { MP_ROM_QSTR(MP_QSTR_SdkError),           MP_ROM_PTR(&mp_type_SdkError) },
    { MP_ROM_QSTR(MP_QSTR_SdkPermissionError), MP_ROM_PTR(&mp_type_SdkPermissionError) },

    /* Open mode constants */
    { MP_ROM_QSTR(MP_QSTR_OPEN_READ),  MP_ROM_INT(JPP_SDK_OPEN_READ)  },
    { MP_ROM_QSTR(MP_QSTR_OPEN_WRITE), MP_ROM_INT(JPP_SDK_OPEN_WRITE) },

    /* Key event constants */
    { MP_ROM_QSTR(MP_QSTR_KEY_NONE),         MP_ROM_INT(JPP_SDK_KEY_NONE) },
    { MP_ROM_QSTR(MP_QSTR_KEY_UP),           MP_ROM_INT(JPP_SDK_KEY_UP) },
    { MP_ROM_QSTR(MP_QSTR_KEY_DOWN),         MP_ROM_INT(JPP_SDK_KEY_DOWN) },
    { MP_ROM_QSTR(MP_QSTR_KEY_LEFT),         MP_ROM_INT(JPP_SDK_KEY_LEFT) },
    { MP_ROM_QSTR(MP_QSTR_KEY_RIGHT),        MP_ROM_INT(JPP_SDK_KEY_RIGHT) },
    { MP_ROM_QSTR(MP_QSTR_KEY_CENTER),       MP_ROM_INT(JPP_SDK_KEY_CENTER) },
    { MP_ROM_QSTR(MP_QSTR_KEY_CENTER_LONG),  MP_ROM_INT(JPP_SDK_KEY_CENTER_LONG) },
    { MP_ROM_QSTR(MP_QSTR_KEY_BACK),         MP_ROM_INT(JPP_SDK_KEY_BACK) },
    { MP_ROM_QSTR(MP_QSTR_KEY_CENTER_HOLD),  MP_ROM_INT(JPP_SDK_KEY_CENTER_HOLD) },
    { MP_ROM_QSTR(MP_QSTR_KEY_CENTER_DOUBLE), MP_ROM_INT(JPP_SDK_KEY_CENTER_DOUBLE) },
    { MP_ROM_QSTR(MP_QSTR_CENTER_CLAIM_NONE),   MP_ROM_INT(JPP_SDK_CENTER_CLAIM_NONE) },
    { MP_ROM_QSTR(MP_QSTR_CENTER_CLAIM_HOLD),   MP_ROM_INT(JPP_SDK_CENTER_CLAIM_HOLD) },
    { MP_ROM_QSTR(MP_QSTR_CENTER_CLAIM_DOUBLE), MP_ROM_INT(JPP_SDK_CENTER_CLAIM_DOUBLE) },

    /* Core */
    { MP_ROM_QSTR(MP_QSTR_set_frame),       MP_ROM_PTR(&mp_sdk_set_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_request_close),   MP_ROM_PTR(&mp_sdk_request_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_log),             MP_ROM_PTR(&mp_sdk_log_obj) },

    /* Device */
    { MP_ROM_QSTR(MP_QSTR_device_status),   MP_ROM_PTR(&mp_sdk_device_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_time),        MP_ROM_PTR(&mp_sdk_get_time_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_dummy_mode),  MP_ROM_PTR(&mp_sdk_is_dummy_mode_obj) },

    /* Scoped files */
    { MP_ROM_QSTR(MP_QSTR_file_read),       MP_ROM_PTR(&mp_sdk_file_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_file_write),      MP_ROM_PTR(&mp_sdk_file_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_file_list),       MP_ROM_PTR(&mp_sdk_file_list_obj) },

    /* Shared files */
    { MP_ROM_QSTR(MP_QSTR_shared_read),     MP_ROM_PTR(&mp_sdk_shared_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_shared_write),    MP_ROM_PTR(&mp_sdk_shared_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_shared_list),     MP_ROM_PTR(&mp_sdk_shared_list_obj) },

    /* Full file handles */
    { MP_ROM_QSTR(MP_QSTR_file_open),       MP_ROM_PTR(&mp_sdk_file_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_handle_read),     MP_ROM_PTR(&mp_sdk_handle_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_handle_write),    MP_ROM_PTR(&mp_sdk_handle_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_handle_list),     MP_ROM_PTR(&mp_sdk_handle_list_obj) },
    { MP_ROM_QSTR(MP_QSTR_handle_close),    MP_ROM_PTR(&mp_sdk_handle_close_obj) },

    /* Input */
    { MP_ROM_QSTR(MP_QSTR_poll_key),        MP_ROM_PTR(&mp_sdk_poll_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_key),        MP_ROM_PTR(&mp_sdk_wait_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_claim_center),    MP_ROM_PTR(&mp_sdk_claim_center_obj) },

    /* High-level UI */
    { MP_ROM_QSTR(MP_QSTR_dialog),          MP_ROM_PTR(&mp_sdk_dialog_obj) },
    { MP_ROM_QSTR(MP_QSTR_list),            MP_ROM_PTR(&mp_sdk_list_obj) },
    { MP_ROM_QSTR(MP_QSTR_input),           MP_ROM_PTR(&mp_sdk_input_obj) },
    { MP_ROM_QSTR(MP_QSTR_INPUT_TEXT),      MP_ROM_INT(JPP_SDK_INPUT_TEXT) },
    { MP_ROM_QSTR(MP_QSTR_INPUT_NUMBER),    MP_ROM_INT(JPP_SDK_INPUT_NUMBER) },
    { MP_ROM_QSTR(MP_QSTR_INPUT_DATE),      MP_ROM_INT(JPP_SDK_INPUT_DATE) },
    { MP_ROM_QSTR(MP_QSTR_INPUT_TIME),      MP_ROM_INT(JPP_SDK_INPUT_TIME) },

    /* BLE scan */
    { MP_ROM_QSTR(MP_QSTR_ble_scan),              MP_ROM_PTR(&mp_sdk_ble_scan_obj) },

    /* BLE advertise */
    { MP_ROM_QSTR(MP_QSTR_ble_advertise_start),   MP_ROM_PTR(&mp_sdk_ble_advertise_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_ble_advertise_stop),    MP_ROM_PTR(&mp_sdk_ble_advertise_stop_obj) },

    /* ESP-NOW */
    { MP_ROM_QSTR(MP_QSTR_espnow_send),          MP_ROM_PTR(&mp_sdk_espnow_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_espnow_recv),          MP_ROM_PTR(&mp_sdk_espnow_recv_obj) },

    /* BLE GATT client */
    { MP_ROM_QSTR(MP_QSTR_ble_connect),           MP_ROM_PTR(&mp_sdk_ble_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_ble_read_char),         MP_ROM_PTR(&mp_sdk_ble_read_char_obj) },
    { MP_ROM_QSTR(MP_QSTR_ble_write_char),        MP_ROM_PTR(&mp_sdk_ble_write_char_obj) },
    { MP_ROM_QSTR(MP_QSTR_ble_disconnect),        MP_ROM_PTR(&mp_sdk_ble_disconnect_obj) },

    /* BLE GATT server */
    { MP_ROM_QSTR(MP_QSTR_ble_service_register),   MP_ROM_PTR(&mp_sdk_ble_service_register_obj) },
    { MP_ROM_QSTR(MP_QSTR_ble_service_unregister), MP_ROM_PTR(&mp_sdk_ble_service_unregister_obj) },

    /* Wakelock */
    { MP_ROM_QSTR(MP_QSTR_wakelock_acquire),    MP_ROM_PTR(&mp_sdk_wakelock_acquire_obj) },
    { MP_ROM_QSTR(MP_QSTR_wakelock_release),    MP_ROM_PTR(&mp_sdk_wakelock_release_obj) },

    /* Buzzer */
    { MP_ROM_QSTR(MP_QSTR_buzzer_play),          MP_ROM_PTR(&mp_sdk_buzzer_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_buzzer_tone),          MP_ROM_PTR(&mp_sdk_buzzer_tone_obj) },
    { MP_ROM_QSTR(MP_QSTR_buzzer_play_sequence), MP_ROM_PTR(&mp_sdk_buzzer_play_sequence_obj) },
    { MP_ROM_QSTR(MP_QSTR_buzzer_play_sequence_async), MP_ROM_PTR(&mp_sdk_buzzer_play_sequence_async_obj) },
    { MP_ROM_QSTR(MP_QSTR_buzzer_stop),          MP_ROM_PTR(&mp_sdk_buzzer_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_SOUND_SUCCESS), MP_ROM_INT(JPP_BUZZER_SOUND_SUCCESS) },
    { MP_ROM_QSTR(MP_QSTR_SOUND_FAILURE), MP_ROM_INT(JPP_BUZZER_SOUND_FAILURE) },
    { MP_ROM_QSTR(MP_QSTR_SOUND_NOTIFY),  MP_ROM_INT(JPP_BUZZER_SOUND_NOTIFY)  },
    { MP_ROM_QSTR(MP_QSTR_SOUND_STARTUP), MP_ROM_INT(JPP_BUZZER_SOUND_STARTUP) },
    { MP_ROM_QSTR(MP_QSTR_SOUND_CLICK),   MP_ROM_INT(JPP_BUZZER_SOUND_CLICK)   },

    /* LED */
    { MP_ROM_QSTR(MP_QSTR_led_set_color), MP_ROM_PTR(&mp_sdk_led_set_color_obj) },
    { MP_ROM_QSTR(MP_QSTR_led_off),       MP_ROM_PTR(&mp_sdk_led_off_obj) },

    /* Background tasks */
    { MP_ROM_QSTR(MP_QSTR_background_register), MP_ROM_PTR(&mp_sdk_background_register_obj) },

    /* HTTP */
    { MP_ROM_QSTR(MP_QSTR_http_request),        MP_ROM_PTR(&mp_sdk_http_request_obj) },
    { MP_ROM_QSTR(MP_QSTR_https_request),       MP_ROM_PTR(&mp_sdk_https_request_obj) },
    { MP_ROM_QSTR(MP_QSTR_net_bind),            MP_ROM_PTR(&mp_sdk_net_bind_obj) },
    { MP_ROM_QSTR(MP_QSTR_net_accept),          MP_ROM_PTR(&mp_sdk_net_accept_obj) },
    { MP_ROM_QSTR(MP_QSTR_net_recv),            MP_ROM_PTR(&mp_sdk_net_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_net_send),            MP_ROM_PTR(&mp_sdk_net_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_net_close),           MP_ROM_PTR(&mp_sdk_net_close_obj) },

    /* Canvas */
    { MP_ROM_QSTR(MP_QSTR_canvas_write),        MP_ROM_PTR(&mp_sdk_canvas_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_canvas_clear),        MP_ROM_PTR(&mp_sdk_canvas_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_canvas_draw_pixel),   MP_ROM_PTR(&mp_sdk_canvas_draw_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_canvas_fullscreen),   MP_ROM_PTR(&mp_sdk_canvas_fullscreen_obj) },

    /* IPC */
    { MP_ROM_QSTR(MP_QSTR_ipc_send),            MP_ROM_PTR(&mp_sdk_ipc_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipc_recv),            MP_ROM_PTR(&mp_sdk_ipc_recv_obj) },

    /* KV store */
    { MP_ROM_QSTR(MP_QSTR_kv_get),              MP_ROM_PTR(&mp_sdk_kv_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_kv_set),              MP_ROM_PTR(&mp_sdk_kv_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_kv_delete),           MP_ROM_PTR(&mp_sdk_kv_delete_obj) },
};
STATIC MP_DEFINE_CONST_DICT(jppsdk_module_globals, jppsdk_module_globals_table);

const mp_obj_module_t jpp_mp_sdk_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&jppsdk_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_jppsdk, jpp_mp_sdk_module);
