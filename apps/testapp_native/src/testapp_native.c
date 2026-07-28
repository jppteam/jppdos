/*
 * testapp_native — exercises every App SDK capability from C.
 *
 * Top-level menu: UI | Device | Files | KV | IPC | HTTP | Network | BLE |
 * Buzzer | System | Exit.  Each section has a sub-menu of individual test
 * cases.  Every test reports pass/fail via a dialog so the result is visible
 * on hardware.
 *
 * Background: the manifest declares a "heartbeat" task (every 300 s).  The
 * System menu's "Background register" item triggers the background.register
 * consent prompt; once granted, the firmware calls jpp_app_task_entry()
 * headlessly while the device idles on the launcher.
 *
 * BLE tests that require a peer device (ble.connect) will show "No peer"
 * if the scan preceding them finds nothing.  All other tests are self-contained.
 * HTTP tests require Wi-Fi to be connected; without it the SDK returns an error
 * which is caught and displayed as expected behaviour.
 */

#include "testapp_native.h"
#include "jpp_sdk_bridge.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Shared helpers                                                               */
/* -------------------------------------------------------------------------- */

/* Show a modal result dialog; clears canvas so pixel leftovers don't bleed. */
static void show(jpp_sdk_context_t *ctx, const char *title, const char *text)
{
    jpp_sdk_ui_result_t r;
    jpp_sdk_canvas_clear(ctx);
    jpp_sdk_dialog(ctx, title, text, &r);
}

/* Show "PASS: <detail>" or "FAIL(<status>): <detail>". */
static void show_result(jpp_sdk_context_t *ctx, const char *title,
                        jpp_sdk_status_t st, const char *detail)
{
    char buf[96];
    if (st == JPP_SDK_STATUS_OK) {
        snprintf(buf, sizeof(buf), "PASS: %s", detail ? detail : "ok");
    } else {
        snprintf(buf, sizeof(buf), "FAIL(%s): %s",
                 jpp_sdk_status_name(st), detail ? detail : "");
    }
    show(ctx, title, buf);
}

/* Run the SDK list helper as a single-select menu; return index or -1 on BACK. */
static int pick(jpp_sdk_context_t *ctx, const char *title,
                const char *const *items, size_t n)
{
    size_t idx[1];
    size_t cnt = 0;
    jpp_sdk_ui_result_t res;
    jpp_sdk_list(ctx, title, items, n, false, idx, 1, &cnt, &res);
    return (res == JPP_SDK_UI_BACK || cnt == 0) ? -1 : (int)idx[0];
}

/* -------------------------------------------------------------------------- */
/* UI tests                                                                     */
/* -------------------------------------------------------------------------- */

static void test_frame(jpp_sdk_context_t *ctx)
{
    const char *lines[] = {
        "Line 1: SDK Test",
        "Line 2: frame",
        "Line 3: content",
        "Line 4: area",
        "Line 5: five",
        "Line 6: six",
        "Line 7: seven",
    };
    jpp_sdk_set_frame(ctx, lines, 7);
    show(ctx, "Frame", "7 lines set. OK?");
}

static void test_dialog(jpp_sdk_context_t *ctx)
{
    jpp_sdk_ui_result_t res;
    jpp_sdk_canvas_clear(ctx);
    jpp_sdk_dialog(ctx, "Dialog test",
                   "Press CENTER to confirm or long-press to dismiss.", &res);
    show(ctx, "Dialog result",
         res == JPP_SDK_UI_OK ? "OK (CENTER)" : "BACK (long)");
}

static void test_list_single(jpp_sdk_context_t *ctx)
{
    const char *items[] = { "Alpha", "Beta", "Gamma", "Delta" };
    int sel = pick(ctx, "Single-select", items, 4);
    char buf[32];
    if (sel < 0) {
        snprintf(buf, sizeof(buf), "BACK");
    } else {
        snprintf(buf, sizeof(buf), "Chose: %s", items[sel]);
    }
    show(ctx, "List result", buf);
}

static void test_list_multi(jpp_sdk_context_t *ctx)
{
    const char *items[] = { "One", "Two", "Three", "Four", "Five" };
    size_t idx[5];
    size_t cnt = 0;
    jpp_sdk_ui_result_t res;
    jpp_sdk_list(ctx, "Multi-select", items, 5, true, idx, 5, &cnt, &res);

    char buf[64] = "Chose: ";
    if (res == JPP_SDK_UI_BACK || cnt == 0) {
        snprintf(buf, sizeof(buf), "BACK / none");
    } else {
        for (size_t i = 0; i < cnt; i++) {
            if (i) strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, items[idx[i]], sizeof(buf) - strlen(buf) - 1);
        }
    }
    show(ctx, "Multi result", buf);
}

static void test_input_text(jpp_sdk_context_t *ctx)
{
    char val[65] = "";
    jpp_sdk_ui_result_t res;
    jpp_sdk_input(ctx, "Enter text", "hello", JPP_SDK_INPUT_TEXT,
                  val, sizeof(val), &res);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s: \"%s\"",
             res == JPP_SDK_UI_OK ? "Got" : "BACK", val);
    show(ctx, "Input TEXT", buf);
}

static void test_input_number(jpp_sdk_context_t *ctx)
{
    char val[65] = "";
    jpp_sdk_ui_result_t res;
    jpp_sdk_input(ctx, "Enter number", "42", JPP_SDK_INPUT_NUMBER,
                  val, sizeof(val), &res);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s: \"%s\"",
             res == JPP_SDK_UI_OK ? "Got" : "BACK", val);
    show(ctx, "Input NUMBER", buf);
}

static void test_input_date(jpp_sdk_context_t *ctx)
{
    char val[65] = "";
    jpp_sdk_ui_result_t res;
    jpp_sdk_input(ctx, "Enter date", "2026-01-01", JPP_SDK_INPUT_DATE,
                  val, sizeof(val), &res);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s: \"%s\"",
             res == JPP_SDK_UI_OK ? "Got" : "BACK", val);
    show(ctx, "Input DATE", buf);
}

static void test_input_time(jpp_sdk_context_t *ctx)
{
    char val[65] = "";
    jpp_sdk_ui_result_t res;
    jpp_sdk_input(ctx, "Enter time", "12:00", JPP_SDK_INPUT_TIME,
                  val, sizeof(val), &res);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s: \"%s\"",
             res == JPP_SDK_UI_OK ? "Got" : "BACK", val);
    show(ctx, "Input TIME", buf);
}

static void test_canvas(jpp_sdk_context_t *ctx)
{
    jpp_sdk_canvas_clear(ctx);
    /* Border rectangle */
    for (uint8_t x = 0; x < 128; x++) {
        jpp_sdk_canvas_draw_pixel(ctx, x, 0,  true);
        jpp_sdk_canvas_draw_pixel(ctx, x, 47, true);
    }
    for (uint8_t y = 0; y < 48; y++) {
        jpp_sdk_canvas_draw_pixel(ctx, 0,   y, true);
        jpp_sdk_canvas_draw_pixel(ctx, 127, y, true);
    }
    /* Diagonal cross */
    for (uint8_t i = 0; i < 48; i++) {
        uint8_t x1 = (uint8_t)(i * 127 / 47);
        uint8_t x2 = (uint8_t)(127 - x1);
        jpp_sdk_canvas_draw_pixel(ctx, x1, i, true);
        jpp_sdk_canvas_draw_pixel(ctx, x2, i, true);
    }
    /* canvas_write: fill row 24 entirely */
    uint8_t row[JPP_SDK_CANVAS_ROW_BYTES];
    memset(row, 0xFF, sizeof(row));
    jpp_sdk_canvas_write(ctx, 24, row);

    const char *lines[] = { "Canvas test", "Border+cross drawn", "Press OK" };
    jpp_sdk_set_frame(ctx, lines, 3);
    jpp_sdk_key_event_t key;
    jpp_sdk_wait_key(ctx, 0, &key);
}

static void test_keys(jpp_sdk_context_t *ctx)
{
    const char *hint[] = { "Key test", "Press 5 keys.", "UP/DN/L/R/CTR/LONG" };
    jpp_sdk_set_frame(ctx, hint, 3);

    static const char *names[] = {
        [JPP_SDK_KEY_NONE]        = "NONE",
        [JPP_SDK_KEY_UP]          = "UP",
        [JPP_SDK_KEY_DOWN]        = "DOWN",
        [JPP_SDK_KEY_LEFT]        = "LEFT",
        [JPP_SDK_KEY_RIGHT]       = "RIGHT",
        [JPP_SDK_KEY_CENTER]      = "CENTER",
        [JPP_SDK_KEY_CENTER_LONG] = "LONG",
    };
    char log[64] = "";
    for (int i = 0; i < 5; i++) {
        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        jpp_sdk_wait_key(ctx, 0, &key);
        if (key < 7) {
            strncat(log, names[key], sizeof(log) - strlen(log) - 1);
            if (i < 4) strncat(log, " ", sizeof(log) - strlen(log) - 1);
        }
    }
    show(ctx, "Keys pressed", log);
}

static void test_file_pick(jpp_sdk_context_t *ctx)
{
    char path[JPP_SDK_PATH_MAX] = "";
    jpp_sdk_ui_result_t res;
    jpp_sdk_file_pick(ctx, path, sizeof(path), &res);
    char buf[JPP_SDK_PATH_MAX + 10];
    snprintf(buf, sizeof(buf), "%s: %s",
             res == JPP_SDK_UI_OK ? "Picked" : "BACK", path);
    show(ctx, "File pick", buf);
}

static void menu_ui(jpp_sdk_context_t *ctx)
{
    static const char *items[] = {
        "Frame (7 lines)",
        "Dialog",
        "List single-sel",
        "List multi-sel",
        "Input TEXT",
        "Input NUMBER",
        "Input DATE",
        "Input TIME",
        "Canvas draw",
        "Key events",
        "File pick",
    };
    for (;;) {
        int sel = pick(ctx, "UI Tests", items,
                       sizeof(items) / sizeof(items[0]));
        if (sel < 0) return;
        switch (sel) {
        case 0:  test_frame(ctx);        break;
        case 1:  test_dialog(ctx);       break;
        case 2:  test_list_single(ctx);  break;
        case 3:  test_list_multi(ctx);   break;
        case 4:  test_input_text(ctx);   break;
        case 5:  test_input_number(ctx); break;
        case 6:  test_input_date(ctx);   break;
        case 7:  test_input_time(ctx);   break;
        case 8:  test_canvas(ctx);       break;
        case 9:  test_keys(ctx);         break;
        case 10: test_file_pick(ctx);    break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Device tests                                                                 */
/* -------------------------------------------------------------------------- */

static void test_device_status(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    jpp_sdk_status_t st = jpp_sdk_device_status(ctx, &r);
    if (st != JPP_SDK_STATUS_OK || !r.ok) {
        show_result(ctx, "device_status", st, r.code);
        return;
    }
    const char *pct      = jpp_broker_result_get(&r, "battery_pct");
    const char *charging = jpp_broker_result_get(&r, "charging");
    char buf[64];
    snprintf(buf, sizeof(buf), "bat=%s%% chg=%s",
             pct ? pct : "?", charging ? charging : "?");
    show_result(ctx, "device_status", JPP_SDK_STATUS_OK, buf);
}

static void test_get_time(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    jpp_sdk_status_t st = jpp_sdk_get_time(ctx, &r);
    if (st != JPP_SDK_STATUS_OK || !r.ok) {
        show_result(ctx, "get_time", st, r.code);
        return;
    }
    const char *text = jpp_broker_result_get(&r, "text");
    show_result(ctx, "get_time", JPP_SDK_STATUS_OK, text ? text : "(no text)");
}

static void menu_device(jpp_sdk_context_t *ctx)
{
    static const char *items[] = { "device_status", "get_time" };
    for (;;) {
        int sel = pick(ctx, "Device Tests", items, 2);
        if (sel < 0) return;
        if (sel == 0) test_device_status(ctx);
        else          test_get_time(ctx);
    }
}

/* -------------------------------------------------------------------------- */
/* File tests                                                                   */
/* -------------------------------------------------------------------------- */

static void test_scoped(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    jpp_sdk_status_t st;

    /* write */
    st = jpp_sdk_file_write(ctx, "test.txt", "hello scoped", &r);
    show_result(ctx, "scoped write", st, r.ok ? "test.txt" : r.code);
    if (st != JPP_SDK_STATUS_OK) return;

    /* read */
    st = jpp_sdk_file_read(ctx, "test.txt", &r);
    const char *txt = jpp_broker_result_get(&r, "text");
    show_result(ctx, "scoped read", st, txt ? txt : r.code);

    /* list root */
    st = jpp_sdk_file_list(ctx, "", &r);
    const char *entries = jpp_broker_result_get(&r, "text");
    show_result(ctx, "scoped list", st, entries ? entries : "(empty)");
}

static void test_shared(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    jpp_sdk_status_t st;

    st = jpp_sdk_shared_write(ctx, "stest.txt", "hello shared", &r);
    show_result(ctx, "shared write", st, r.ok ? "stest.txt" : r.code);
    if (st != JPP_SDK_STATUS_OK) return;

    st = jpp_sdk_shared_read(ctx, "stest.txt", &r);
    const char *txt = jpp_broker_result_get(&r, "text");
    show_result(ctx, "shared read", st, txt ? txt : r.code);

    st = jpp_sdk_shared_list(ctx, "", &r);
    const char *entries = jpp_broker_result_get(&r, "text");
    show_result(ctx, "shared list", st, entries ? entries : "(empty)");
}

static void test_full_handle(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    jpp_sdk_handle_t h;

    /* open for write */
    jpp_sdk_status_t st = jpp_sdk_file_open(
        ctx, "/sd/apps/testapp_native/handle_test.txt", JPP_SDK_OPEN_WRITE, &h);
    show_result(ctx, "file_open (write)", st,
                st == JPP_SDK_STATUS_OK ? "handle ok" : "denied/err");
    if (st != JPP_SDK_STATUS_OK) return;

    st = jpp_sdk_handle_write(ctx, h, "handle write test\n", &r);
    show_result(ctx, "handle_write", st, r.ok ? "written" : r.code);
    jpp_sdk_handle_close(ctx, h);

    /* open for read */
    st = jpp_sdk_file_open(
        ctx, "/sd/apps/testapp_native/handle_test.txt", JPP_SDK_OPEN_READ, &h);
    if (st != JPP_SDK_STATUS_OK) {
        show_result(ctx, "file_open (read)", st, "denied/err");
        return;
    }
    st = jpp_sdk_handle_read(ctx, h, &r);
    const char *txt = jpp_broker_result_get(&r, "text");
    show_result(ctx, "handle_read", st, txt ? txt : "(empty)");
    jpp_sdk_handle_close(ctx, h);

    /* list the apps dir */
    st = jpp_sdk_file_open(
        ctx, "/sd/apps/testapp_native", JPP_SDK_OPEN_READ, &h);
    if (st == JPP_SDK_STATUS_OK) {
        st = jpp_sdk_handle_list(ctx, h, &r);
        const char *entries = jpp_broker_result_get(&r, "text");
        show_result(ctx, "handle_list", st, entries ? entries : "(empty)");
        jpp_sdk_handle_close(ctx, h);
    }
}

static void menu_files(jpp_sdk_context_t *ctx)
{
    static const char *items[] = {
        "Scoped r/w/list",
        "Shared r/w/list",
        "Full handle r/w/list",
    };
    for (;;) {
        int sel = pick(ctx, "File Tests", items, 3);
        if (sel < 0) return;
        switch (sel) {
        case 0: test_scoped(ctx);      break;
        case 1: test_shared(ctx);      break;
        case 2: test_full_handle(ctx); break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* KV store tests                                                               */
/* -------------------------------------------------------------------------- */

static void test_kv(jpp_sdk_context_t *ctx)
{
    char val[JPP_SDK_KV_VALUE_MAX];

    jpp_sdk_status_t st = jpp_sdk_kv_set(ctx, "test_key", "hello_kv");
    show_result(ctx, "kv_set", st, "test_key=hello_kv");
    if (st != JPP_SDK_STATUS_OK) return;

    val[0] = '\0';
    st = jpp_sdk_kv_get(ctx, "test_key", val, sizeof(val));
    show_result(ctx, "kv_get", st, val[0] ? val : "(empty)");

    st = jpp_sdk_kv_delete(ctx, "test_key");
    show_result(ctx, "kv_delete", st, "test_key");

    val[0] = '\0';
    st = jpp_sdk_kv_get(ctx, "test_key", val, sizeof(val));
    show_result(ctx, "kv_get (deleted)", st,
                st == JPP_SDK_STATUS_OK ? val : "key gone (expected)");
}

/* -------------------------------------------------------------------------- */
/* IPC tests                                                                    */
/* -------------------------------------------------------------------------- */

static void test_ipc(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;

    jpp_sdk_status_t st = jpp_sdk_ipc_send(ctx, "testapp_native", "ping from self", &r);
    show_result(ctx, "ipc_send (to self)", st, st == JPP_SDK_STATUS_OK ? "sent" : r.code);
    if (st != JPP_SDK_STATUS_OK) return;

    char payload[JPP_SDK_IPC_PAYLOAD_MAX];
    payload[0] = '\0';
    st = jpp_sdk_ipc_recv(ctx, payload, sizeof(payload), &r);
    if (st == JPP_SDK_STATUS_OK && r.ok) {
        char buf[64];
        const char *sender = jpp_broker_result_get(&r, "sender");
        snprintf(buf, sizeof(buf), "from=%.20s: %.36s",
                 sender ? sender : "?", payload);
        show_result(ctx, "ipc_recv", JPP_SDK_STATUS_OK, buf);
    } else {
        show_result(ctx, "ipc_recv", st, "no messages");
    }
}

/* -------------------------------------------------------------------------- */
/* HTTP tests                                                                   */
/* -------------------------------------------------------------------------- */

static void test_http_get(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    jpp_sdk_status_t st = jpp_sdk_http_request(
        ctx, "GET", "http://neverssl.com/", NULL, &r);
    if (st != JPP_SDK_STATUS_OK || !r.ok) {
        show_result(ctx, "http GET", st, r.ok ? "" : (r.code ? r.code : "err"));
        return;
    }
    const char *code = jpp_broker_result_get(&r, "status_code");
    char buf[32];
    snprintf(buf, sizeof(buf), "HTTP %s", code ? code : "?");
    show_result(ctx, "http GET", JPP_SDK_STATUS_OK, buf);
}

static void test_http_post(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    jpp_sdk_status_t st = jpp_sdk_http_request(
        ctx, "POST", "http://httpbin.org/post", "{\"test\":1}", &r);
    if (st != JPP_SDK_STATUS_OK || !r.ok) {
        show_result(ctx, "http POST", st, r.ok ? "" : (r.code ? r.code : "err"));
        return;
    }
    const char *code = jpp_broker_result_get(&r, "status_code");
    char buf[32];
    snprintf(buf, sizeof(buf), "HTTP %s", code ? code : "?");
    show_result(ctx, "http POST", JPP_SDK_STATUS_OK, buf);
}

static void menu_http(jpp_sdk_context_t *ctx)
{
    static const char *items[] = { "GET neverssl.com", "POST httpbin.org" };
    for (;;) {
        int sel = pick(ctx, "HTTP Tests", items, 2);
        if (sel < 0) return;
        if (sel == 0) test_http_get(ctx);
        else          test_http_post(ctx);
    }
}

/* -------------------------------------------------------------------------- */
/* Network tests                                                                */
/* -------------------------------------------------------------------------- */

/* Bind port 8266, echo one message back, then close everything. */
static void test_net_echo(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    jpp_sdk_status_t st = jpp_sdk_net_bind(ctx, 8266u, &r);
    if (st != JPP_SDK_STATUS_OK || !r.ok) {
        show_result(ctx, "net_bind", st, r.ok ? "" : (r.code ? r.code : "err"));
        return;
    }
    show(ctx, "net echo :8266", "Waiting 15s for a connection...");

    int sock = -1;
    st = jpp_sdk_net_accept(ctx, 15000u, &sock, &r);
    if (st != JPP_SDK_STATUS_OK || !r.ok) {
        show_result(ctx, "net_accept", st, r.ok ? "" : (r.code ? r.code : "err"));
    } else if (sock < 0) {
        show_result(ctx, "net_accept", JPP_SDK_STATUS_OK, "no connection (timeout)");
    } else {
        static uint8_t rx[256];
        size_t n = 0u;
        st = jpp_sdk_net_recv(ctx, sock, rx, sizeof(rx), &n, 5000u, &r);
        if (st == JPP_SDK_STATUS_OK && r.ok && n > 0u) {
            jpp_sdk_net_send(ctx, sock, (const uint8_t *)"echo: ", 6u, &r);
            jpp_sdk_net_send(ctx, sock, rx, n, &r);
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "echoed %u byte(s)", (unsigned)n);
        show_result(ctx, "net echo", st, buf);
        jpp_sdk_net_close(ctx, sock, &r);
    }
    jpp_sdk_net_close(ctx, -1, &r);   /* close the listener */
}

/* -------------------------------------------------------------------------- */
/* BLE tests                                                                    */
/* -------------------------------------------------------------------------- */

static void test_ble_scan(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    jpp_sdk_ble_scan_result_t results[JPP_SDK_BLE_SCAN_MAX];
    size_t cnt = 0;
    show(ctx, "BLE scan", "Scanning 1s...");
    jpp_sdk_status_t st = jpp_sdk_ble_scan(ctx, 1000, results,
                                            JPP_SDK_BLE_SCAN_MAX, &cnt, &r);
    if (st != JPP_SDK_STATUS_OK || !r.ok) {
        show_result(ctx, "ble_scan", st, r.code ? r.code : "denied");
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "Found %zu device(s)", cnt);
    show_result(ctx, "ble_scan", JPP_SDK_STATUS_OK, buf);
}

static void test_ble_advertise(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    /* 3-byte test payload: AD type 0xFF (manufacturer), data 0xAB 0xCD */
    static const uint8_t payload[] = { 0x03, 0xFF, 0xAB, 0xCD };

    jpp_sdk_status_t st = jpp_sdk_ble_advertise_start(ctx, payload, sizeof(payload), &r);
    show_result(ctx, "ble_advertise_start", st, r.ok ? "advertising" : (r.code ? r.code : "err"));
    if (st != JPP_SDK_STATUS_OK) return;

    show(ctx, "Advertising", "Running 1s, then stop.");
    /* Brief delay — firmware tick-based; use a blocking wait_key with short timeout */
    jpp_sdk_key_event_t key;
    jpp_sdk_wait_key(ctx, 1000, &key);

    st = jpp_sdk_ble_advertise_stop(ctx, &r);
    show_result(ctx, "ble_advertise_stop", st, r.ok ? "stopped" : "err");
}

static void test_ble_connectable(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    jpp_sdk_status_t st = jpp_sdk_ble_set_connectable(ctx, true, &r);
    show_result(ctx, "ble_set_connectable(true)", st, r.ok ? "ok" : "err");
    if (st == JPP_SDK_STATUS_OK) {
        st = jpp_sdk_ble_set_connectable(ctx, false, &r);
        show_result(ctx, "ble_set_connectable(false)", st, r.ok ? "ok" : "err");
    }
}

static void test_ble_host(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;

    jpp_sdk_status_t st = jpp_sdk_ble_service_register(
        ctx, JPP_SDK_BLE_HOST_SVC_UUID, &r);
    show_result(ctx, "ble_service_register", st, r.ok ? "registered" : (r.code ? r.code : "denied"));
    if (st != JPP_SDK_STATUS_OK) return;

    /* Publish a value on the TX characteristic */
    static const uint8_t tx_data[] = { 0x01, 0x02, 0x03, 0x04 };
    st = jpp_sdk_ble_host_set_value(ctx, tx_data, sizeof(tx_data), &r);
    show_result(ctx, "ble_host_set_value", st, r.ok ? "published 4 bytes" : "err");

    /* Wait for an RX write with a 2-second timeout */
    show(ctx, "ble_host_wait_write", "Waiting 2s for peer write...");
    uint8_t rx[64];
    size_t rx_len = sizeof(rx);
    bool received = false;
    st = jpp_sdk_ble_host_wait_write(ctx, rx, &rx_len, 2000, &received, &r);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s (%zu bytes)",
             received ? "received" : "timeout", received ? rx_len : 0u);
    show_result(ctx, "ble_host_wait_write", st, buf);

    st = jpp_sdk_ble_host_clear(ctx, &r);
    show_result(ctx, "ble_host_clear", st, r.ok ? "cleared" : "err");

    st = jpp_sdk_ble_service_unregister(ctx, &r);
    show_result(ctx, "ble_service_unregister", st, r.ok ? "unregistered" : "err");
}

static void test_ble_connect(jpp_sdk_context_t *ctx)
{
    /* Scan briefly to find a connectable peer */
    jpp_broker_result_t r;
    jpp_sdk_ble_scan_result_t results[JPP_SDK_BLE_SCAN_MAX];
    size_t cnt = 0;
    show(ctx, "ble.connect", "Scanning 2s for peer...");
    jpp_sdk_status_t st = jpp_sdk_ble_scan(ctx, 2000, results,
                                            JPP_SDK_BLE_SCAN_MAX, &cnt, &r);
    if (st != JPP_SDK_STATUS_OK || !r.ok || cnt == 0) {
        show(ctx, "ble.connect", "No peer found. Skipping connect test.");
        return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "Connect to %s?", results[0].address);
    jpp_sdk_ui_result_t res;
    jpp_sdk_dialog(ctx, "ble.connect", buf, &res);
    if (res != JPP_SDK_UI_OK) return;

    jpp_sdk_ble_conn_t conn;
    st = jpp_sdk_ble_connect(ctx, results[0].address, &conn);
    show_result(ctx, "ble_connect", st,
                st == JPP_SDK_STATUS_OK ? "connected" : "failed");
    if (st != JPP_SDK_STATUS_OK) return;

    /* Try reading the host TX characteristic on the peer */
    st = jpp_sdk_ble_read_char(ctx, conn,
                                JPP_SDK_BLE_HOST_SVC_UUID, JPP_SDK_BLE_HOST_TX_UUID, &r);
    const char *val = jpp_broker_result_get(&r, "value");
    show_result(ctx, "ble_read_char", st, val ? val : "(no value / no svc)");

    /* Write to peer RX characteristic */
    st = jpp_sdk_ble_write_char(ctx, conn,
                                 JPP_SDK_BLE_HOST_SVC_UUID, JPP_SDK_BLE_HOST_RX_UUID,
                                 "ping", &r);
    show_result(ctx, "ble_write_char", st, r.ok ? "written" : "err/no svc");

    st = jpp_sdk_ble_disconnect(ctx, conn);
    show_result(ctx, "ble_disconnect", st, "done");
}

static void menu_ble(jpp_sdk_context_t *ctx)
{
    static const char *items[] = {
        "Scan (1s)",
        "Advertise (1s)",
        "Set connectable",
        "Host svc + wait",
        "Connect to peer",
    };
    for (;;) {
        int sel = pick(ctx, "BLE Tests", items, 5);
        if (sel < 0) return;
        switch (sel) {
        case 0: test_ble_scan(ctx);        break;
        case 1: test_ble_advertise(ctx);   break;
        case 2: test_ble_connectable(ctx); break;
        case 3: test_ble_host(ctx);        break;
        case 4: test_ble_connect(ctx);     break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Buzzer tests                                                                 */
/* -------------------------------------------------------------------------- */

static void test_buzzer(jpp_sdk_context_t *ctx)
{
    static const struct { const char *label; jpp_buzzer_sound_t sound; } sounds[] = {
        { "SUCCESS", JPP_BUZZER_SOUND_SUCCESS },
        { "FAILURE", JPP_BUZZER_SOUND_FAILURE },
        { "NOTIFY",  JPP_BUZZER_SOUND_NOTIFY  },
        { "STARTUP", JPP_BUZZER_SOUND_STARTUP },
        { "CLICK",   JPP_BUZZER_SOUND_CLICK   },
    };
    static const char *items[] = {
        "SUCCESS sound",
        "FAILURE sound",
        "NOTIFY sound",
        "STARTUP sound",
        "CLICK sound",
        "Tone 440 Hz/300ms",
        "Scale sequence",
        "Stop (mid-tone)",
    };
    for (;;) {
        int sel = pick(ctx, "Buzzer Tests", items, 8);
        if (sel < 0) return;
        jpp_sdk_status_t st;
        char buf[32];
        if (sel < 5) {
            st = jpp_sdk_buzzer_play(ctx, sounds[sel].sound);
            snprintf(buf, sizeof(buf), "played %s", sounds[sel].label);
            show_result(ctx, "buzzer_play", st, buf);
        } else if (sel == 5) {
            st = jpp_sdk_buzzer_tone(ctx, 440, 300);
            show_result(ctx, "buzzer_tone", st, "440 Hz 300ms");
        } else if (sel == 6) {
            static const jpp_buzzer_note_t scale[] = {
                {262, 150}, {294, 150}, {330, 150}, {349, 150},
                {392, 150}, {440, 150}, {494, 150}, {523, 300},
            };
            st = jpp_sdk_buzzer_play_sequence(ctx, scale,
                                               sizeof(scale) / sizeof(scale[0]));
            show_result(ctx, "buzzer_sequence", st, "C major scale");
        } else {
            jpp_sdk_buzzer_tone(ctx, 880, 5000); /* start long tone */
            st = jpp_sdk_buzzer_stop(ctx);       /* stop it immediately */
            show_result(ctx, "buzzer_stop", st, "stopped 880Hz");
        }
    }
}

/* -------------------------------------------------------------------------- */
/* System tests                                                                 */
/* -------------------------------------------------------------------------- */

static void test_wakelock(jpp_sdk_context_t *ctx)
{
    jpp_sdk_status_t st = jpp_sdk_wakelock_acquire(ctx);
    show_result(ctx, "wakelock_acquire", st, "held");
    if (st != JPP_SDK_STATUS_OK) return;
    st = jpp_sdk_wakelock_release(ctx);
    show_result(ctx, "wakelock_release", st, "released");
}

static void test_log(jpp_sdk_context_t *ctx)
{
    jpp_sdk_status_t st = jpp_sdk_log(ctx, "testapp_native.test_log");
    show_result(ctx, "sdk_log", st, "testapp_native.test_log");
}

static void test_background_register(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    jpp_sdk_status_t st = jpp_sdk_background_register(ctx, &r);
    show_result(ctx, "background_register", st,
                st == JPP_SDK_STATUS_OK ? "granted; schedule syncs at exit"
                                        : r.code);
}

static void menu_system(jpp_sdk_context_t *ctx)
{
    static const char *items[] = {
        "Wakelock acq/rel", "Log event", "Background register"
    };
    for (;;) {
        int sel = pick(ctx, "System Tests", items, 3);
        if (sel < 0) return;
        if (sel == 0)      test_wakelock(ctx);
        else if (sel == 1) test_log(ctx);
        else               test_background_register(ctx);
    }
}

/* -------------------------------------------------------------------------- */
/* Top-level entry                                                              */
/* -------------------------------------------------------------------------- */

void testapp_native_run(jpp_sdk_context_t *ctx)
{
    static const char *items[] = {
        "UI",
        "Device",
        "Files",
        "KV Store",
        "IPC",
        "HTTP",
        "Network",
        "BLE",
        "Buzzer",
        "System",
        "Exit",
    };

    jpp_sdk_wakelock_acquire(ctx);
    show(ctx, "SDK Test (C)", "All SDK caps. Use menu to run tests.");

    for (;;) {
        int sel = pick(ctx, "SDK Test (C)", items, 11);
        if (sel < 0 || sel == 10) break;
        switch (sel) {
        case 0: menu_ui(ctx);       break;
        case 1: menu_device(ctx);   break;
        case 2: menu_files(ctx);    break;
        case 3: test_kv(ctx);       break;
        case 4: test_ipc(ctx);      break;
        case 5: menu_http(ctx);     break;
        case 6: test_net_echo(ctx); break;
        case 7: menu_ble(ctx);      break;
        case 8: test_buzzer(ctx);   break;
        case 9: menu_system(ctx);   break;
        }
    }

    jpp_sdk_wakelock_release(ctx);
    jpp_sdk_request_close(ctx);
}

/* Headless background entry — the loader resolves jpp_app_task_entry and the
   firmware calls it (no UI available) when a scheduled task is due. */
void testapp_native_run_task(jpp_sdk_context_t *ctx, const char *name)
{
    char path[64];
    char text[48] = "(time unavailable)";
    jpp_broker_result_t r;

    if (jpp_sdk_get_time(ctx, &r) == JPP_SDK_STATUS_OK && r.ok) {
        const char *t = jpp_broker_result_get(&r, "text");
        if (t != NULL) {
            snprintf(text, sizeof(text), "%s", t);
        }
    }
    snprintf(path, sizeof(path), "bg_%s.txt", name);
    jpp_sdk_file_write(ctx, path, text, &r);
    jpp_sdk_log(ctx, "testapp_native.on_task");
}
