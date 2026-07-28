"""
testapp_mp — exercises every App SDK capability from MicroPython.

Menu layout mirrors testapp_native (C version):
  UI | Device | Files | KV | IPC | HTTP | Network | BLE | Buzzer | System | Exit

Background: the manifest declares a "heartbeat" task (every 300 s). The
System menu's "Background register" item triggers the background.register
consent prompt; once granted, the firmware runs on_task("heartbeat")
headlessly while the device idles on the launcher.

The app runs as a blocking sequence from on_idle() so the flow is identical
to the C version: each test calls blocking SDK helpers (dialog, list, input)
which wait for the user before returning.

Note: ble_host_set_value / ble_host_wait_write / ble_host_clear /
ble_set_connectable are not exposed in the jppsdk Python module; those
functions are covered by the C version only.
"""

import jppsdk

# --------------------------------------------------------------------------- #
# Helpers                                                                      #
# --------------------------------------------------------------------------- #

def _show(sdk, title, text):
    sdk.canvas_clear()
    sdk.dialog(text, title)


def _show_result(sdk, title, ok, detail):
    prefix = "PASS" if ok else "FAIL"
    _show(sdk, title, f"{prefix}: {detail}")


def _pick(sdk, title, items):
    """Single-select menu; returns index or None on BACK."""
    return sdk.list(items, title=title)


# --------------------------------------------------------------------------- #
# UI tests                                                                     #
# --------------------------------------------------------------------------- #

def test_frame(sdk):
    sdk.set_frame([
        "Line 1: SDK Test",
        "Line 2: frame",
        "Line 3: content",
        "Line 4: area",
        "Line 5: five",
        "Line 6: six",
        "Line 7: seven",
    ])
    _show(sdk, "Frame", "7 lines set. OK?")


def test_dialog(sdk):
    sdk.canvas_clear()
    ok = sdk.dialog("Press CENTER to confirm or long-press to dismiss.",
                    "Dialog test")
    _show(sdk, "Dialog result", "OK (CENTER)" if ok else "BACK (long)")


def test_list_single(sdk):
    items = ["Alpha", "Beta", "Gamma", "Delta"]
    sel = sdk.list(items, title="Single-select")
    if sel is None:
        _show(sdk, "List result", "BACK")
    else:
        _show(sdk, "List result", f"Chose: {items[sel]}")


def test_list_multi(sdk):
    items = ["One", "Two", "Three", "Four", "Five"]
    sel = sdk.list(items, title="Multi-select", multiselect=True)
    if sel is None or len(sel) == 0:
        _show(sdk, "Multi result", "BACK / none")
    else:
        chosen = ", ".join(items[i] for i in sel)
        _show(sdk, "Multi result", f"Chose: {chosen}")


def test_input_text(sdk):
    val = sdk.input("Enter text", placeholder="hello", type=jppsdk.INPUT_TEXT)
    _show(sdk, "Input TEXT", f'Got: "{val}"' if val is not None else "BACK")


def test_input_number(sdk):
    val = sdk.input("Enter number", placeholder="42", type=jppsdk.INPUT_NUMBER)
    _show(sdk, "Input NUMBER", f'Got: "{val}"' if val is not None else "BACK")


def test_input_date(sdk):
    val = sdk.input("Enter date", placeholder="2026-01-01", type=jppsdk.INPUT_DATE)
    _show(sdk, "Input DATE", f'Got: "{val}"' if val is not None else "BACK")


def test_input_time(sdk):
    val = sdk.input("Enter time", placeholder="12:00", type=jppsdk.INPUT_TIME)
    _show(sdk, "Input TIME", f'Got: "{val}"' if val is not None else "BACK")


def test_canvas(sdk):
    sdk.canvas_clear()
    # Border rectangle
    for x in range(128):
        sdk.canvas_draw_pixel(x, 0, True)
        sdk.canvas_draw_pixel(x, 47, True)
    for y in range(48):
        sdk.canvas_draw_pixel(0, y, True)
        sdk.canvas_draw_pixel(127, y, True)
    # Diagonal cross
    for i in range(48):
        x1 = i * 127 // 47
        x2 = 127 - x1
        sdk.canvas_draw_pixel(x1, i, True)
        sdk.canvas_draw_pixel(x2, i, True)
    # canvas_write: fill row 24 solid
    sdk.canvas_write(24, bytes([0xFF] * 16))
    sdk.set_frame(["Canvas test", "Border+cross drawn", "Press OK"])
    sdk.wait_key(0)


def test_keys(sdk):
    sdk.set_frame(["Key test", "Press 5 keys.", "UP/DN/L/R/CTR/LONG"])
    names = {
        jppsdk.KEY_NONE:        "NONE",
        jppsdk.KEY_UP:          "UP",
        jppsdk.KEY_DOWN:        "DOWN",
        jppsdk.KEY_LEFT:        "LEFT",
        jppsdk.KEY_RIGHT:       "RIGHT",
        jppsdk.KEY_CENTER:      "CENTER",
        jppsdk.KEY_CENTER_LONG: "LONG",
    }
    pressed = []
    for _ in range(5):
        k = sdk.wait_key(0)
        pressed.append(names.get(k, "?"))
    _show(sdk, "Keys pressed", " ".join(pressed))


def menu_ui(sdk):
    items = [
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
    ]
    while True:
        sel = _pick(sdk, "UI Tests", items)
        if sel is None:
            return
        handlers = [
            test_frame, test_dialog, test_list_single, test_list_multi,
            test_input_text, test_input_number, test_input_date, test_input_time,
            test_canvas, test_keys,
        ]
        handlers[sel](sdk)


# --------------------------------------------------------------------------- #
# Device tests                                                                 #
# --------------------------------------------------------------------------- #

def test_device_status(sdk):
    try:
        info = sdk.device_status()
        bat = info.get("battery_pct", "?")
        chg = info.get("charging", "?")
        _show_result(sdk, "device_status", True, f"bat={bat}% chg={chg}")
    except jppsdk.SdkError as e:
        _show_result(sdk, "device_status", False, str(e))


def test_get_time(sdk):
    try:
        t = sdk.get_time()
        _show_result(sdk, "get_time", True, t)
    except jppsdk.SdkError as e:
        _show_result(sdk, "get_time", False, str(e))


def menu_device(sdk):
    items = ["device_status", "get_time"]
    while True:
        sel = _pick(sdk, "Device Tests", items)
        if sel is None:
            return
        if sel == 0:
            test_device_status(sdk)
        else:
            test_get_time(sdk)


# --------------------------------------------------------------------------- #
# File tests                                                                   #
# --------------------------------------------------------------------------- #

def test_scoped(sdk):
    try:
        sdk.file_write("test.txt", "hello scoped")
        _show_result(sdk, "scoped write", True, "test.txt")
    except jppsdk.SdkError as e:
        _show_result(sdk, "scoped write", False, str(e))
        return
    try:
        d = sdk.file_read("test.txt")
        _show_result(sdk, "scoped read", True, d.get("text", "(empty)"))
    except jppsdk.SdkError as e:
        _show_result(sdk, "scoped read", False, str(e))
    try:
        d = sdk.file_list("")
        _show_result(sdk, "scoped list", True, d.get("text", "(empty)"))
    except jppsdk.SdkError as e:
        _show_result(sdk, "scoped list", False, str(e))


def test_shared(sdk):
    try:
        sdk.shared_write("stest.txt", "hello shared")
        _show_result(sdk, "shared write", True, "stest.txt")
    except jppsdk.SdkError as e:
        _show_result(sdk, "shared write", False, str(e))
        return
    try:
        d = sdk.shared_read("stest.txt")
        _show_result(sdk, "shared read", True, d.get("text", "(empty)"))
    except jppsdk.SdkError as e:
        _show_result(sdk, "shared read", False, str(e))
    try:
        d = sdk.shared_list("")
        _show_result(sdk, "shared list", True, d.get("text", "(empty)"))
    except jppsdk.SdkError as e:
        _show_result(sdk, "shared list", False, str(e))


def test_full_handle(sdk):
    path = "/sd/apps/testapp_mp/handle_test.txt"
    try:
        h = sdk.file_open(path, jppsdk.OPEN_WRITE)
        _show_result(sdk, "file_open (write)", True, "handle ok")
    except jppsdk.SdkError as e:
        _show_result(sdk, "file_open (write)", False, str(e))
        return
    try:
        sdk.handle_write(h, "handle write test\n")
        _show_result(sdk, "handle_write", True, "written")
    except jppsdk.SdkError as e:
        _show_result(sdk, "handle_write", False, str(e))
    sdk.handle_close(h)

    try:
        h = sdk.file_open(path, jppsdk.OPEN_READ)
        d = sdk.handle_read(h)
        _show_result(sdk, "handle_read", True, d.get("text", "(empty)"))
        sdk.handle_close(h)
    except jppsdk.SdkError as e:
        _show_result(sdk, "handle_read", False, str(e))

    try:
        h = sdk.file_open("/sd/apps/testapp_mp", jppsdk.OPEN_READ)
        d = sdk.handle_list(h)
        _show_result(sdk, "handle_list", True, d.get("text", "(empty)"))
        sdk.handle_close(h)
    except jppsdk.SdkError as e:
        _show_result(sdk, "handle_list", False, str(e))


def menu_files(sdk):
    items = ["Scoped r/w/list", "Shared r/w/list", "Full handle r/w/list"]
    while True:
        sel = _pick(sdk, "File Tests", items)
        if sel is None:
            return
        if sel == 0:
            test_scoped(sdk)
        elif sel == 1:
            test_shared(sdk)
        else:
            test_full_handle(sdk)


# --------------------------------------------------------------------------- #
# KV store tests                                                               #
# --------------------------------------------------------------------------- #

def test_kv(sdk):
    try:
        sdk.kv_set("test_key", "hello_kv")
        _show_result(sdk, "kv_set", True, "test_key=hello_kv")
    except jppsdk.SdkError as e:
        _show_result(sdk, "kv_set", False, str(e))
        return
    val = sdk.kv_get("test_key")
    _show_result(sdk, "kv_get", val is not None, val if val else "(missing)")
    try:
        sdk.kv_delete("test_key")
        _show_result(sdk, "kv_delete", True, "test_key deleted")
    except jppsdk.SdkError as e:
        _show_result(sdk, "kv_delete", False, str(e))
    gone = sdk.kv_get("test_key")
    _show_result(sdk, "kv_get (deleted)", gone is None, "key gone (expected)" if gone is None else f"still: {gone}")


# --------------------------------------------------------------------------- #
# IPC tests                                                                    #
# --------------------------------------------------------------------------- #

def test_ipc(sdk):
    try:
        sdk.ipc_send("testapp_mp", "ping from self")
        _show_result(sdk, "ipc_send (to self)", True, "sent")
    except jppsdk.SdkError as e:
        _show_result(sdk, "ipc_send (to self)", False, str(e))
        return
    msg = sdk.ipc_recv()
    if msg is None:
        _show_result(sdk, "ipc_recv", False, "no message")
    else:
        payload, sender = msg
        _show_result(sdk, "ipc_recv", True, f"from={sender}: {payload}")


# --------------------------------------------------------------------------- #
# HTTP tests                                                                   #
# --------------------------------------------------------------------------- #

def test_http_get(sdk):
    try:
        r = sdk.http_request("GET", "http://neverssl.com/")
        code = r.get("status_code", "?")
        _show_result(sdk, "http GET", True, f"HTTP {code}")
    except jppsdk.SdkError as e:
        _show_result(sdk, "http GET", False, str(e))


def test_http_post(sdk):
    try:
        r = sdk.http_request("POST", "http://httpbin.org/post", '{"test":1}')
        code = r.get("status_code", "?")
        _show_result(sdk, "http POST", True, f"HTTP {code}")
    except jppsdk.SdkError as e:
        _show_result(sdk, "http POST", False, str(e))


def menu_http(sdk):
    items = ["GET neverssl.com", "POST httpbin.org"]
    while True:
        sel = _pick(sdk, "HTTP Tests", items)
        if sel is None:
            return
        if sel == 0:
            test_http_get(sdk)
        else:
            test_http_post(sdk)


# --------------------------------------------------------------------------- #
# Network tests                                                                #
# --------------------------------------------------------------------------- #

def test_net_echo(sdk):
    """Bind port 8266, echo one message back, then close everything."""
    try:
        sdk.net_bind(8266)
    except jppsdk.SdkError as e:
        _show_result(sdk, "net_bind", False, str(e))
        return
    _show(sdk, "net echo :8266", "Waiting 15s for a connection...")
    try:
        sock = sdk.net_accept(15000)
        if sock is None:
            _show_result(sdk, "net_accept", True, "no connection (timeout)")
            return
        data = sdk.net_recv(sock, 256, 5000)
        if data:
            sdk.net_send(sock, b"echo: " + data)
        _show_result(sdk, "net echo", True, f"echoed {len(data)} byte(s)")
        sdk.net_close(sock)
    except jppsdk.SdkError as e:
        _show_result(sdk, "net echo", False, str(e))
    finally:
        try:
            sdk.net_close(-1)   # close the listener
        except jppsdk.SdkError:
            pass


# --------------------------------------------------------------------------- #
# BLE tests                                                                    #
# --------------------------------------------------------------------------- #

def test_ble_scan(sdk):
    _show(sdk, "BLE scan", "Scanning 1s...")
    try:
        results = sdk.ble_scan(1000)
        _show_result(sdk, "ble_scan", True, f"Found {len(results)} device(s)")
    except jppsdk.SdkError as e:
        _show_result(sdk, "ble_scan", False, str(e))


def test_ble_advertise(sdk):
    payload = bytes([0x03, 0xFF, 0xAB, 0xCD])
    try:
        sdk.ble_advertise_start(payload)
        _show_result(sdk, "ble_advertise_start", True, "advertising")
    except jppsdk.SdkError as e:
        _show_result(sdk, "ble_advertise_start", False, str(e))
        return
    _show(sdk, "Advertising", "Running 1s, then stop.")
    sdk.wait_key(1000)
    try:
        sdk.ble_advertise_stop()
        _show_result(sdk, "ble_advertise_stop", True, "stopped")
    except jppsdk.SdkError as e:
        _show_result(sdk, "ble_advertise_stop", False, str(e))


def test_ble_service(sdk):
    try:
        sdk.ble_service_register("4a505300-0000-0000-0000-000000000000")
        _show_result(sdk, "ble_service_register", True, "registered")
    except jppsdk.SdkError as e:
        _show_result(sdk, "ble_service_register", False, str(e))
        return
    try:
        sdk.ble_service_unregister()
        _show_result(sdk, "ble_service_unregister", True, "unregistered")
    except jppsdk.SdkError as e:
        _show_result(sdk, "ble_service_unregister", False, str(e))


def test_ble_connect(sdk):
    _show(sdk, "ble.connect", "Scanning 2s for peer...")
    try:
        results = sdk.ble_scan(2000)
    except jppsdk.SdkError as e:
        _show_result(sdk, "ble_scan", False, str(e))
        return

    if not results:
        _show(sdk, "ble.connect", "No peer found. Skipping.")
        return

    peer = results[0]
    ok = sdk.dialog(f"Connect to {peer['address']}?", "ble.connect")
    if not ok:
        return

    try:
        conn = sdk.ble_connect(peer["address"])
        _show_result(sdk, "ble_connect", True, "connected")
    except jppsdk.SdkError as e:
        _show_result(sdk, "ble_connect", False, str(e))
        return

    try:
        d = sdk.ble_read_char(conn,
                              "4a505300-0000-0000-0000-000000000000",
                              "4a505301-0000-0000-0000-000000000000")
        _show_result(sdk, "ble_read_char", True, d.get("value", "(no value)"))
    except jppsdk.SdkError as e:
        _show_result(sdk, "ble_read_char", False, str(e))

    try:
        sdk.ble_write_char(conn,
                           "4a505300-0000-0000-0000-000000000000",
                           "4a505302-0000-0000-0000-000000000000",
                           "ping")
        _show_result(sdk, "ble_write_char", True, "written")
    except jppsdk.SdkError as e:
        _show_result(sdk, "ble_write_char", False, str(e))

    try:
        sdk.ble_disconnect(conn)
        _show_result(sdk, "ble_disconnect", True, "done")
    except jppsdk.SdkError as e:
        _show_result(sdk, "ble_disconnect", False, str(e))


def menu_ble(sdk):
    items = [
        "Scan (1s)",
        "Advertise (1s)",
        "Host svc reg/unreg",
        "Connect to peer",
    ]
    while True:
        sel = _pick(sdk, "BLE Tests", items)
        if sel is None:
            return
        if sel == 0:
            test_ble_scan(sdk)
        elif sel == 1:
            test_ble_advertise(sdk)
        elif sel == 2:
            test_ble_service(sdk)
        else:
            test_ble_connect(sdk)


# --------------------------------------------------------------------------- #
# Buzzer tests                                                                 #
# --------------------------------------------------------------------------- #

_SOUNDS = [
    ("SUCCESS", jppsdk.SOUND_SUCCESS),
    ("FAILURE", jppsdk.SOUND_FAILURE),
    ("NOTIFY",  jppsdk.SOUND_NOTIFY),
    ("STARTUP", jppsdk.SOUND_STARTUP),
    ("CLICK",   jppsdk.SOUND_CLICK),
]

def test_buzzer(sdk):
    items = [
        "SUCCESS sound",
        "FAILURE sound",
        "NOTIFY sound",
        "STARTUP sound",
        "CLICK sound",
        "Tone 440 Hz/300ms",
        "Scale sequence",
        "Stop (mid-tone)",
    ]
    while True:
        sel = _pick(sdk, "Buzzer Tests", items)
        if sel is None:
            return
        try:
            if sel < 5:
                label, sound = _SOUNDS[sel]
                sdk.buzzer_play(sound)
                _show_result(sdk, "buzzer_play", True, f"played {label}")
            elif sel == 5:
                sdk.buzzer_tone(440, 300)
                _show_result(sdk, "buzzer_tone", True, "440 Hz 300ms")
            elif sel == 6:
                scale = [
                    (262, 150), (294, 150), (330, 150), (349, 150),
                    (392, 150), (440, 150), (494, 150), (523, 300),
                ]
                sdk.buzzer_play_sequence(scale)
                _show_result(sdk, "buzzer_sequence", True, "C major scale")
            else:
                sdk.buzzer_tone(880, 5000)
                sdk.buzzer_stop()
                _show_result(sdk, "buzzer_stop", True, "stopped 880Hz")
        except jppsdk.SdkError as e:
            _show_result(sdk, items[sel], False, str(e))


# --------------------------------------------------------------------------- #
# System tests                                                                 #
# --------------------------------------------------------------------------- #

def test_wakelock(sdk):
    try:
        sdk.wakelock_acquire()
        _show_result(sdk, "wakelock_acquire", True, "held")
        sdk.wakelock_release()
        _show_result(sdk, "wakelock_release", True, "released")
    except jppsdk.SdkError as e:
        _show_result(sdk, "wakelock", False, str(e))


def test_log(sdk):
    try:
        sdk.log("testapp_mp.test_log")
        _show_result(sdk, "sdk_log", True, "testapp_mp.test_log")
    except jppsdk.SdkError as e:
        _show_result(sdk, "sdk_log", False, str(e))


def test_background_register(sdk):
    try:
        sdk.background_register()
        _show_result(sdk, "background_register", True,
                     "granted; schedule syncs at exit")
    except jppsdk.SdkError as e:
        _show_result(sdk, "background_register", False, str(e))


def menu_system(sdk):
    items = ["Wakelock acq/rel", "Log event", "Background register"]
    while True:
        sel = _pick(sdk, "System Tests", items)
        if sel is None:
            return
        if sel == 0:
            test_wakelock(sdk)
        elif sel == 1:
            test_log(sdk)
        else:
            test_background_register(sdk)


# --------------------------------------------------------------------------- #
# App entry                                                                    #
# --------------------------------------------------------------------------- #

def on_task(name):
    """Headless background entry point — the firmware imports this module and
    calls on_task(name) when a scheduled task is due (no UI available)."""
    jppsdk.log("on_task: " + name)
    jppsdk.file_write("bg_" + name + ".txt", jppsdk.get_time())


def create_app(sdk):
    return TestAppMP(sdk)


class TestAppMP:
    def __init__(self, sdk):
        self.sdk = sdk
        self._started = False

    def on_start(self):
        self.sdk.wakelock_acquire()

    def on_idle(self):
        if self._started:
            return
        self._started = True
        self._run()
        self.sdk.request_close()

    def handle_action(self, key):
        pass

    def on_stop(self):
        self.sdk.wakelock_release()

    def _run(self):
        sdk = self.sdk
        _show(sdk, "SDK Test (MP)", "All SDK caps. Use menu to run tests.")

        top_items = [
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
        ]
        while True:
            sel = _pick(sdk, "SDK Test (MP)", top_items)
            if sel is None or sel == 10:
                break
            if sel == 0:
                menu_ui(sdk)
            elif sel == 1:
                menu_device(sdk)
            elif sel == 2:
                menu_files(sdk)
            elif sel == 3:
                test_kv(sdk)
            elif sel == 4:
                test_ipc(sdk)
            elif sel == 5:
                menu_http(sdk)
            elif sel == 6:
                test_net_echo(sdk)
            elif sel == 7:
                menu_ble(sdk)
            elif sel == 8:
                test_buzzer(sdk)
            elif sel == 9:
                menu_system(sdk)
