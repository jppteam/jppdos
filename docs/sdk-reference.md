# SDK reference

**Contents**

| Category | Functions |
|----------|-----------|
| [Types and constants](#types-and-constants) | [`jpp_sdk_status_t`](#c--jpp_sdk_status_t) · [`jpp_sdk_key_event_t`](#c--jpp_sdk_key_event_t) · [Python constants](#python--jppsdk-constants) · [`jpp_broker_result_t`](#c--jpp_broker_result_t) |
| [App control](#app-control) | [`set_frame`](#set_frame) · [`request_close`](#request_close) · [`log`](#log) |
| [Key input](#key-input) | [`poll_key`](#poll_key) · [`wait_key`](#wait_key) · [`push_key`](#push_key-c-only) |
| [Canvas](#canvas) | [`canvas_write`](#canvas_write) · [`canvas_draw_pixel`](#canvas_draw_pixel) · [`canvas_clear`](#canvas_clear) · [`canvas_fullscreen`](#canvas_fullscreen) |
| [UI helpers](#ui-helpers) | [`dialog`](#dialog) · [`list`](#list) · [`input`](#input) · [`confirm`](#confirm) · [`file_pick`](#file_pick) · [`wrap_text`](#wrap_text-c-only) |
| [Wakelock](#wakelock) | [`wakelock_acquire`](#wakelock_acquire) · [`wakelock_release`](#wakelock_release) |
| [Buzzer](#buzzer) | [`buzzer_play`](#buzzer_play) · [`buzzer_tone`](#buzzer_tone) · [`buzzer_play_sequence`](#buzzer_play_sequence) · [`buzzer_play_sequence_async`](#buzzer_play_sequence_async) · [`buzzer_stop`](#buzzer_stop) |
| [Device status](#device-status) | [`device_status`](#device_status) · [`get_time`](#get_time) |
| [Scoped file I/O](#scoped-file-io) | [`file_read`](#file_read) · [`file_write`](#file_write) · [`file_list`](#file_list) |
| [Shared file I/O](#shared-file-io) | [`shared_read`](#shared_read) · [`shared_write`](#shared_write) · [`shared_list`](#shared_list) |
| [Full file access](#full-file-access) ⚠ `files.full` | [`file_open`](#file_open) · [`handle_read`](#handle_read) · [`handle_write`](#handle_write) · [`handle_list`](#handle_list) · [`handle_close`](#handle_close) |
| [Key-value store](#key-value-store) | [`kv_get`](#kv_get) · [`kv_set`](#kv_set) · [`kv_delete`](#kv_delete) |
| [IPC](#ipc) | [`ipc_send`](#ipc_send) · [`ipc_recv`](#ipc_recv) |
| [HTTP](#http) ⚠ `http.request` | [`http_request`](#http_request) |
| [Network / TCP server](#network-tcp-server) ⚠ `network.bind` | [`net_bind`](#net_bind) · [`net_accept`](#net_accept) · [`net_recv`](#net_recv) · [`net_send`](#net_send) · [`net_close`](#net_close) |
| [BLE scan](#ble-scan) ⚠ `ble.scan` | [`ble_scan`](#ble_scan) |
| [BLE advertise](#ble-advertise) ⚠ `ble.advertise` | [`ble_advertise_start`](#ble_advertise_start) · [`ble_advertise_stop`](#ble_advertise_stop) · [`ble_set_connectable`](#ble_set_connectable) |
| [BLE GATT client](#ble-gatt-client) ⚠ `ble.connect` | [`ble_connect`](#ble_connect) · [`ble_read_char`](#ble_read_char) · [`ble_write_char`](#ble_write_char) · [`ble_disconnect`](#ble_disconnect) |
| [BLE GATT server](#ble-gatt-server) ⚠ `ble.host` | [`ble_service_register`](#ble_service_register) · [`ble_service_unregister`](#ble_service_unregister) · [`ble_host_set_value`](#ble_host_set_value) · [`ble_host_wait_write`](#ble_host_wait_write) · [`ble_host_clear`](#ble_host_clear) |
| [Background registration](#background-registration) ⚠ `background.register` | [`background_register`](#background_register) |
| [Code modules](#code-modules-native-apps-only) (native only) | [`module_load`](#jpp_sdk_module_load) · [`module_run`](#jpp_sdk_module_run) · [`module_unload`](#jpp_sdk_module_unload) |
| [Resource limits](#resource-limits) | — |

⚠ = capability required

---

Complete documentation for every App SDK call. Functions are organized by category. For each entry:

- **C** shows the full signature from `jpp_sdk_bridge.h`
- **Python** shows the `jppsdk` module binding
- Capability requirements are listed — if absent, the call is **ungated** (no declaration needed)
- Return values describe both the C status code and any output parameters/Python return value
- Notes cover caveats that are not obvious from the signature

Unless otherwise noted, a Python binding raises `jppsdk.SdkError` on any non-OK status and `jppsdk.SdkPermissionError` (a subclass) on `ACCESS_DENIED`.

---

## Types and constants

### C — `jpp_sdk_status_t`

| Constant | Value | Meaning |
|----------|-------|---------|
| `JPP_SDK_OK` | 0 | Success |
| `JPP_SDK_INVALID_ARGUMENT` | 1 | Bad parameter (NULL, out of range, etc.) |
| `JPP_SDK_INVALID_STATE` | 2 | Called in the wrong state (e.g. no active session) |
| `JPP_SDK_ACCESS_DENIED` | 3 | Capability not granted — handle gracefully, do not crash |
| `JPP_SDK_TEXT_TRUNCATED` | 4 | Output was written but clipped to fit |
| `JPP_SDK_BROKER_ERROR` | 5 | Internal broker failure |
| `JPP_SDK_HANDLE_LIMIT` | 6 | Maximum number of open file handles reached (limit: 4) |
| `JPP_SDK_INVALID_HANDLE` | 7 | Handle was not opened or has been closed |
| `JPP_SDK_BLE_CONN_LIMIT` | 8 | Maximum BLE connections reached |
| `JPP_SDK_INVALID_BLE_CONN` | 9 | BLE connection handle is not valid |
| `JPP_SDK_NO_DATA` | 10 | Requested item does not exist (absent KV key, empty IPC mailbox) |

### C — `jpp_sdk_key_event_t`

| Constant | Meaning |
|----------|---------|
| `JPP_SDK_KEY_NONE` | No key (poll returned empty, or wait timed out) |
| `JPP_SDK_KEY_UP` | D-pad up |
| `JPP_SDK_KEY_DOWN` | D-pad down |
| `JPP_SDK_KEY_LEFT` | D-pad left |
| `JPP_SDK_KEY_RIGHT` | D-pad right |
| `JPP_SDK_KEY_CENTER` | D-pad center (short press) |
| `JPP_SDK_KEY_CENTER_LONG` | D-pad center long-press — the universal "back/cancel" gesture |

### Python — `jppsdk` constants

```python
# Key events (match C enum values)
KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_CENTER, KEY_CENTER_LONG

# File open modes
OPEN_READ = 0
OPEN_WRITE = 1

# Input types
INPUT_TEXT = 0    # On-screen QWERTY keyboard
INPUT_NUMBER = 1  # Digit keyboard with - and .
INPUT_DATE = 2    # Field spinner, returns "YYYY-MM-DD"
INPUT_TIME = 3    # Field spinner, returns "HH:MM:SS"

# Predefined buzzer sounds
SOUND_SUCCESS, SOUND_FAILURE, SOUND_NOTIFY, SOUND_STARTUP, SOUND_CLICK
```

### C — `jpp_broker_result_t`

Several SDK calls return results through a `jpp_broker_result_t *result` output parameter. This struct carries key-value pairs from the broker:

| Field | Set by | Description |
|-------|--------|-------------|
| `text` | File I/O, `get_time`, directory listing | The primary text payload |
| `status_code` | `http_request` | HTTP response status code (int as string) |
| `body` | `http_request` | HTTP response body |
| `port` | `net_bind` | The bound port number |
| `closed` | `net_recv` | Set to `"1"` if the peer closed the connection |
| `sender` | `ipc_recv` | The sender's `app_id` |

Access fields with `jpp_broker_result_get(result, "text")`, which returns a `const char *` or `NULL` if the field is absent.

---

## App control

### `set_frame`

Sets the text content displayed on the OLED above the canvas area.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_set_frame(jpp_sdk_context_t *ctx,
                                    const char *const *lines,
                                    size_t line_count);
```
```python
jppsdk.set_frame(lines: list[str]) -> None
```

**Parameters:**

| Name | Description |
|------|-------------|
| `lines` | Array of text rows. Each row fits ~21 characters; longer strings are silently truncated. |
| `line_count` | Number of rows. Maximum 7. Extra rows beyond the limit are dropped. |

**Returns:** `JPP_SDK_OK`, or `JPP_SDK_TEXT_TRUNCATED` if any line was clipped.

**Notes:**
- Calling `set_frame` clears fullscreen canvas mode. If you were using `canvas_fullscreen(true)`, re-enable it after each `set_frame` call.
- Row 0 acts as the title — if non-empty, a 1-px signature rule is drawn below it.
- The frame persists until the next `set_frame`. You do not need to call it on every idle tick; call it only when content changes.

---

### `request_close`

Signals the firmware to close the app and return to the launcher.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_request_close(jpp_sdk_context_t *ctx);
```
```python
jppsdk.request_close() -> None
```

**Notes:**
- For MicroPython apps, the firmware calls `on_stop` and then tears down the runtime. You can still do cleanup after `request_close()` returns — the app is not immediately killed.
- For native apps, call `request_close` then return from `jpp_app_entry`. The firmware closes the app when the function returns; you can also return without calling it and the firmware will close cleanly.
- Calling `request_close` inside `on_idle` sets a flag; the app runs until `on_stop` completes.

---

### `log`

Emits a named event to the device log (visible over serial monitor).

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_log(jpp_sdk_context_t *ctx,
                              const char *event_name);
```
```python
jppsdk.log(event_name: str) -> None
```

**Notes:** Useful for debugging. Log lines appear on UART0 with the tag `app_log`.

---

## Key input

### `poll_key`

Returns the next key event from the queue without blocking. Returns `KEY_NONE` immediately if no key is pending.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_poll_key(jpp_sdk_context_t *ctx,
                                   jpp_sdk_key_event_t *out_event);
```
```python
jppsdk.poll_key() -> int
```

**Returns:**
- C: `JPP_SDK_OK`; `*out_event` is the key constant (may be `JPP_SDK_KEY_NONE`)
- Python: the key constant as an `int`

**Notes:** Use this in `on_idle` where you want to check for keys but not block the idle loop.

---

### `wait_key`

Blocks until a key event is available or the timeout elapses.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_wait_key(jpp_sdk_context_t *ctx,
                                   uint32_t timeout_ms,
                                   jpp_sdk_key_event_t *out_event);
```
```python
jppsdk.wait_key(timeout_ms: int) -> int
```

**Parameters:**

| Name | Description |
|------|-------------|
| `timeout_ms` | Maximum wait time in milliseconds. Pass `0` to wait indefinitely. |

**Returns:** The key constant. `JPP_SDK_KEY_NONE` if the timeout elapsed with no key.

**Notes:** Use this as the main loop driver in native apps or in MicroPython blocking sequences. Pass a short timeout (e.g. 100 ms) when you need periodic work between key events.

---

### `push_key` (C only)

Injects a synthetic key event into the queue. Useful for testing and for UI helpers that need to replay a key.

**Capability:** None

```c
void jpp_sdk_push_key(jpp_sdk_context_t *ctx, jpp_sdk_key_event_t event);
```

---

## Canvas

The canvas is a **128×48 pixel area** occupying OLED pages 2–7 (below the frame text). In fullscreen mode it expands to **128×64 pixels** covering the entire display.

Each pixel row is 16 bytes (128 bits). In each byte, the most significant bit is the leftmost pixel: byte 0 bits `[7..0]` map to pixels x=0..7, byte 1 to x=8..15, and so on.

### `canvas_write`

Write a complete row of pixels to the canvas.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_canvas_write(jpp_sdk_context_t *ctx,
                                       uint8_t row,
                                       const uint8_t *pixels);
```
```python
jppsdk.canvas_write(row: int, pixels: bytes) -> None
```

**Parameters:**

| Name | Description |
|------|-------------|
| `row` | Pixel row. 0–47 in windowed mode; 0–63 in fullscreen mode. |
| `pixels` | Exactly 16 bytes. MSB of byte 0 = leftmost pixel of the row. |

**Notes:** Canvas writes are buffered and flushed to the display on the next render tick. You can write multiple rows before the display updates.

---

### `canvas_draw_pixel`

Set or clear a single pixel.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_canvas_draw_pixel(jpp_sdk_context_t *ctx,
                                            uint8_t x, uint8_t y,
                                            bool on);
```
```python
jppsdk.canvas_draw_pixel(x: int, y: int, on: bool) -> None
```

**Parameters:**

| Name | Description |
|------|-------------|
| `x` | Horizontal position, 0–127 (left to right). |
| `y` | Vertical position. 0–47 windowed; 0–63 fullscreen. |
| `on` | `true` to light the pixel; `false` to clear it. |

---

### `canvas_clear`

Zero the entire canvas (turn all pixels off).

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_canvas_clear(jpp_sdk_context_t *ctx);
```
```python
jppsdk.canvas_clear() -> None
```

---

### `canvas_fullscreen`

Toggle the fullscreen canvas mode.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_canvas_fullscreen(jpp_sdk_context_t *ctx, bool on);
```
```python
jppsdk.canvas_fullscreen(on: bool) -> None
```

**Parameters:**

| Name | Description |
|------|-------------|
| `on` | `true` to enable the 128×64 full-display canvas; `false` to return to the 128×48 windowed canvas. |

**Notes:**
- Enabling fullscreen clears the canvas and hides the frame text and signature rule.
- `set_frame` and all modal UI helpers (`dialog`, `list`, `input`) switch back to windowed mode. Call `canvas_fullscreen(true)` again after any of those calls.
- Games should call `canvas_fullscreen(true)` once at startup and re-enable it after any modal.

---

## UI helpers

These are **blocking modal** calls. They take over the display and d-pad until the user resolves the prompt, then return. The control scheme is consistent across all helpers: d-pad moves focus, CENTER selects/confirms, CENTER long-press cancels.

All titled modals draw the signature rule under the title row, matching the system Settings and launcher style.

### `dialog`

Show a message with an OK button. The user can confirm or press BACK to dismiss.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_dialog(jpp_sdk_context_t *ctx,
                                 const char *title,
                                 const char *text,
                                 jpp_sdk_ui_result_t *out_result);
```
```python
jppsdk.dialog(text: str, title: str = None) -> bool
```

**Parameters:**

| Name | Description |
|------|-------------|
| `title` | Optional title displayed in row 0 with a rule below it. Pass `NULL`/`None` for no title. |
| `text` | The message body. Long text is automatically word-wrapped across available rows. |
| `out_result` | C only: receives `JPP_SDK_UI_OK` (user pressed OK) or `JPP_SDK_UI_BACK` (dismissed). |

**Returns:**
- C: `JPP_SDK_OK`; check `*out_result` for the user's choice.
- Python: `True` if the user pressed OK; `False` if they pressed BACK.

---

### `list`

Show a scrollable list of items for the user to select.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_list(jpp_sdk_context_t *ctx,
                               const char *title,
                               const char *const *items,
                               size_t item_count,
                               bool multiselect,
                               size_t *out_indices,
                               size_t out_capacity,
                               size_t *out_count,
                               jpp_sdk_ui_result_t *out_result);
```
```python
jppsdk.list(items: list[str],
            title: str = None,
            multiselect: bool = False) -> int | list[int] | None
```

**Parameters:**

| Name | Description |
|------|-------------|
| `title` | Optional title. |
| `items` | The list of options to display. |
| `item_count` | Number of items (C only). |
| `multiselect` | If `true`, the user can check multiple items before confirming with a trailing "Done" row. If `false`, selection is immediate on CENTER. |
| `out_indices` | C only: array receiving the selected indices. |
| `out_capacity` | C only: size of `out_indices` array. |
| `out_count` | C only: number of indices written to `out_indices`. |

**Returns:**
- C: `JPP_SDK_OK`; check `*out_result` (`JPP_SDK_UI_OK` or `JPP_SDK_UI_BACK`) and `*out_count`.
- Python, single-select: the chosen index as `int`, or `None` if cancelled.
- Python, multiselect: a `list[int]` of checked indices, or `None` if cancelled.

**Notes:** Long item names scroll as marquees when focused.

---

### `input`

Show a text input prompt. The input type selects the keyboard layout.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_input(jpp_sdk_context_t *ctx,
                                const char *title,
                                const char *placeholder,
                                jpp_sdk_input_type_t type,
                                char *out_value,
                                size_t value_buf_len,
                                jpp_sdk_ui_result_t *out_result);
```
```python
jppsdk.input(title: str,
             placeholder: str = None,
             type: int = jppsdk.INPUT_TEXT) -> str | None
```

**Parameters:**

| Name | Description |
|------|-------------|
| `title` | Prompt title shown at the top. |
| `placeholder` | Pre-filled text, or `NULL`/`None` to start empty. |
| `type` | Input mode — see below. |
| `out_value` | C only: buffer for the returned string. Max `value_buf_len - 1` characters. |
| `value_buf_len` | C only: size of `out_value` including the null terminator. |

**Input types:**

| Type | Keyboard | Returns |
|------|----------|---------|
| `INPUT_TEXT` | Full QWERTY with shift, space, backspace, done, cancel | The typed string |
| `INPUT_NUMBER` | Digit keys with `-` and `.` | The typed string |
| `INPUT_DATE` | Field spinner: year / month / day; LEFT/RIGHT to move fields, UP/DOWN to adjust; **123** for manual entry, **now** to fill from RTC | `"YYYY-MM-DD"` |
| `INPUT_TIME` | Field spinner: hour / minute / second; same controls as `INPUT_DATE` | `"HH:MM:SS"` |

**Returns:**
- C: `JPP_SDK_OK`; check `*out_result`.
- Python: the entered string, or `None` if the user cancelled.

**Notes:** Cancelling (BACK or cancel key) returns `None`/`JPP_SDK_UI_BACK`. The returned string is limited to 64 characters.

---

### `confirm`

Show a customizable Deny/Allow prompt. This is the same consent surface used by capability and `files.full` path prompts.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_confirm(jpp_sdk_context_t *ctx,
                                  const char *title,
                                  const char *const *body_lines,
                                  size_t body_count,
                                  bool default_allow,
                                  bool *out_allow);
```

**Parameters:**

| Name | Description |
|------|-------------|
| `title` | Prompt title. |
| `body_lines` | Array of body text rows. |
| `body_count` | Number of body rows. |
| `default_allow` | Which button (`Deny`/`Allow`) is focused by default. |
| `out_allow` | Receives the user's decision: `true` = Allow, `false` = Deny. |

**Notes:** There is no Python binding for `confirm` — the MicroPython test app uses `dialog` for similar prompts. `confirm` is primarily useful in native apps that implement their own consent flows.

---

### `file_pick`

Launch a full SD card file browser and return the path the user selects.

**Capability:** `files.full`

```c
jpp_sdk_status_t jpp_sdk_file_pick(jpp_sdk_context_t *ctx,
                                    char *out_path,
                                    size_t out_path_len,
                                    jpp_sdk_ui_result_t *out_result);
```

**Notes:** The browser starts at the SD root (`/sd`). Directories appear with a `/` suffix; `..` navigates up. Long names scroll as marquees. `out_path` receives the absolute path of the selected file. Returns `JPP_SDK_UI_BACK` if the user cancels without selecting a file.

There is no direct Python equivalent; use `file_open` with a known path for MicroPython apps that need full access.

---

### `wrap_text` (C only)

Word-wrap a string into fixed-width frame lines.

```c
size_t jpp_sdk_wrap_text(const char *text,
                          char lines[][JPP_SDK_FRAME_TEXT_MAX],
                          size_t max_lines);
```

**Returns:** Number of lines produced. Use this to split a long message before passing it to `set_frame` or a modal helper.

---

## Wakelock

The wakelock prevents the device from dimming the OLED or entering deep sleep while your app is active. It does not prevent the user from forcing sleep via the power button.

### `wakelock_acquire`

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_wakelock_acquire(jpp_sdk_context_t *ctx);
```
```python
jppsdk.wakelock_acquire() -> None
```

Acquire the wakelock. The device will not dim or sleep while the lock is held.

---

### `wakelock_release`

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_wakelock_release(jpp_sdk_context_t *ctx);
```
```python
jppsdk.wakelock_release() -> None
```

Release the wakelock. The OS resumes normal power management.

**Notes:** The wakelock is automatically released when the app exits. Acquire it for as short a time as possible — holding it indefinitely drains the battery.

---

## Buzzer

### `buzzer_play`

Play a predefined sound effect.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_buzzer_play(jpp_sdk_context_t *ctx,
                                      jpp_buzzer_sound_t sound);
```
```python
jppsdk.buzzer_play(sound: int) -> None
```

**Parameters:**

| `sound` constant | Description |
|-----------------|-------------|
| `SOUND_SUCCESS` / `JPP_BUZZER_SOUND_SUCCESS` | Short rising confirmation tone |
| `SOUND_FAILURE` / `JPP_BUZZER_SOUND_FAILURE` | Short descending error tone |
| `SOUND_NOTIFY` / `JPP_BUZZER_SOUND_NOTIFY` | Brief notification chime |
| `SOUND_STARTUP` / `JPP_BUZZER_SOUND_STARTUP` | The system startup jingle |
| `SOUND_CLICK` / `JPP_BUZZER_SOUND_CLICK` | Single soft click |

---

### `buzzer_tone`

Play a single tone for a fixed duration. Blocks until the tone finishes.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_buzzer_tone(jpp_sdk_context_t *ctx,
                                      uint32_t freq_hz,
                                      uint32_t duration_ms);
```
```python
jppsdk.buzzer_tone(freq_hz: int, duration_ms: int) -> None
```

**Parameters:**

| Name | Description |
|------|-------------|
| `freq_hz` | Tone frequency in Hz. Pass `0` for silence (a rest/pause). |
| `duration_ms` | Duration in milliseconds. |

---

### `buzzer_play_sequence`

Play a sequence of notes. Blocks until the entire sequence finishes.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_buzzer_play_sequence(jpp_sdk_context_t *ctx,
                                               const jpp_buzzer_note_t *notes,
                                               size_t count);
```
```python
jppsdk.buzzer_play_sequence(notes: list[tuple[int, int]]) -> None
```

**Parameters:**

| Name | Description |
|------|-------------|
| `notes` | Array of `(freq_hz, duration_ms)` pairs. `freq_hz=0` is a rest. |
| `count` | Number of notes (C only). |

**Notes:** Preempts any currently playing async sequence.

**C example:**

```c
jpp_buzzer_note_t melody[] = {
    { 440, 150 }, { 0, 50 }, { 523, 150 }, { 659, 300 }
};
jpp_sdk_buzzer_play_sequence(ctx, melody, 4);
```

**Python example:**

```python
jppsdk.buzzer_play_sequence([(440, 150), (0, 50), (523, 150), (659, 300)])
```

---

### `buzzer_play_sequence_async`

Play a note sequence without blocking the caller. Returns immediately; the sequence plays in a dedicated task.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_buzzer_play_sequence_async(jpp_sdk_context_t *ctx,
                                                     const jpp_buzzer_note_t *notes,
                                                     size_t count);
```
```python
jppsdk.buzzer_play_sequence_async(notes: list[tuple[int, int]]) -> None
```

**Notes:**
- The sequence is **copied** before the call returns — you do not need to keep `notes` alive.
- Starting a new async sequence (or calling `buzzer_stop`) preempts the previous one within one note period. This makes it suitable for cycling through jingle previews.
- `buzzer_play_sequence` (blocking) also preempts any running async sequence.

---

### `buzzer_stop`

Stop any currently playing sound immediately.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_buzzer_stop(jpp_sdk_context_t *ctx);
```
```python
jppsdk.buzzer_stop() -> None
```

---

## Device status

### `device_status`

Return the current battery and charging state.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_device_status(jpp_sdk_context_t *ctx,
                                        jpp_broker_result_t *result);
```
```python
jppsdk.device_status() -> dict
```

**Returns:**
- C: `JPP_SDK_OK`; `result` fields `battery_pct` (integer as string, `-1` if unknown) and `charging` (`"1"` if charging, `"0"` if not, `"-1"` if unknown).
- Python: `{"battery_pct": int, "charging": str}`.

---

### `get_time`

Return the current date and time from the RTC.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_get_time(jpp_sdk_context_t *ctx,
                                   jpp_broker_result_t *result);
```
```python
jppsdk.get_time() -> str
```

**Returns:**
- C: `JPP_SDK_OK`; `result->text` is `"YYYY-MM-DD HH:mm"`.
- Python: `"YYYY-MM-DD HH:mm"` as a string.

**Notes:** Reads the DS1307 I²C RTC. If the RTC has not been set, it may return a nonsensical date. Time is always in the device's configured timezone.

---

## Scoped file I/O

These calls are sandboxed to the app's own directory: `/sd/apps/<app_id>/`. Paths are **relative** — a path of `"data/notes.txt"` accesses `/sd/apps/<app_id>/data/notes.txt`. Parent directory traversal (`../`) is rejected.

**Capability:** None

### `file_read`

Read the entire contents of a file into memory.

```c
jpp_sdk_status_t jpp_sdk_file_read(jpp_sdk_context_t *ctx,
                                    const char *relative_path,
                                    jpp_broker_result_t *result);
```
```python
jppsdk.file_read(path: str) -> dict
```

**Returns:**
- C: `JPP_SDK_OK`; `result->text` is the file contents as a null-terminated string.
- Python: `{"text": str}` — access the contents with `result["text"]`.

---

### `file_write`

Write a string to a file, replacing its contents. Creates the file (and any parent directories) if necessary.

```c
jpp_sdk_status_t jpp_sdk_file_write(jpp_sdk_context_t *ctx,
                                     const char *relative_path,
                                     const char *text,
                                     jpp_broker_result_t *result);
```
```python
jppsdk.file_write(path: str, text: str) -> None
```

**Notes:** Writing is atomic via a temp-file rename — a power loss during the write will not corrupt the existing file.

---

### `file_list`

List the contents of a directory.

```c
jpp_sdk_status_t jpp_sdk_file_list(jpp_sdk_context_t *ctx,
                                    const char *relative_dir,
                                    jpp_broker_result_t *result);
```
```python
jppsdk.file_list(dir: str) -> dict
```

**Returns:**
- C: `JPP_SDK_OK`; `result->text` is a newline-separated list of filenames. Individual entries are also in `result` as `entry_0`, `entry_1`, etc.
- Python: `{"text": str}` — split on `"\n"` to get individual names.

---

## Shared file I/O

These calls work identically to scoped file I/O but target `/sd/shared/<app_id>/` instead. Files here can be seen and read by other apps that specifically open that path with `files.full`.

**Capability:** None

```c
jpp_sdk_status_t jpp_sdk_shared_read(jpp_sdk_context_t *ctx,
                                      const char *relative_path,
                                      jpp_broker_result_t *result);

jpp_sdk_status_t jpp_sdk_shared_write(jpp_sdk_context_t *ctx,
                                       const char *relative_path,
                                       const char *text,
                                       jpp_broker_result_t *result);

jpp_sdk_status_t jpp_sdk_shared_list(jpp_sdk_context_t *ctx,
                                      const char *relative_dir,
                                      jpp_broker_result_t *result);
```
```python
jppsdk.shared_read(path: str) -> dict
jppsdk.shared_write(path: str, text: str) -> None
jppsdk.shared_list(dir: str) -> dict
```

---

## Full file access

These calls allow access to **any path on the SD card** under `/sd/`. Each path the app opens requires individual user approval — a Deny/Allow prompt appears before the handle is granted.

**Capability:** `files.full` (Tier 2 — per-session grant, prompted on first use)

### `file_open`

Open a file and obtain a handle. A user approval prompt is shown for the requested path and mode before the handle is granted.

```c
jpp_sdk_status_t jpp_sdk_file_open(jpp_sdk_context_t *ctx,
                                    const char *path,
                                    jpp_sdk_open_mode_t mode,
                                    jpp_sdk_handle_t *out_handle);
```
```python
jppsdk.file_open(path: str, mode: int) -> int
```

**Parameters:**

| Name | Description |
|------|-------------|
| `path` | Absolute path under `/sd/`, e.g. `"/sd/shared/other_app/data.txt"`. |
| `mode` | `OPEN_READ` (0) or `OPEN_WRITE` (1). |
| `out_handle` / return | An integer handle used in subsequent `handle_*` calls. |

**Limits:** Maximum 4 open handles per session. Returns `JPP_SDK_HANDLE_LIMIT` if exceeded.

---

### `handle_read`

Read the entire contents of an open file handle.

```c
jpp_sdk_status_t jpp_sdk_handle_read(jpp_sdk_context_t *ctx,
                                      jpp_sdk_handle_t handle,
                                      jpp_broker_result_t *result);
```
```python
jppsdk.handle_read(handle: int) -> dict
```

**Returns:** `result->text` / `result["text"]` — the file contents.

---

### `handle_write`

Write a string to an open file handle (must be opened with `OPEN_WRITE`).

```c
jpp_sdk_status_t jpp_sdk_handle_write(jpp_sdk_context_t *ctx,
                                       jpp_sdk_handle_t handle,
                                       const char *text,
                                       jpp_broker_result_t *result);
```
```python
jppsdk.handle_write(handle: int, text: str) -> None
```

---

### `handle_list`

List directory contents for a handle opened on a directory path.

```c
jpp_sdk_status_t jpp_sdk_handle_list(jpp_sdk_context_t *ctx,
                                      jpp_sdk_handle_t handle,
                                      jpp_broker_result_t *result);
```
```python
jppsdk.handle_list(handle: int) -> dict
```

---

### `handle_close`

Close an open handle and release the slot.

```c
jpp_sdk_status_t jpp_sdk_handle_close(jpp_sdk_context_t *ctx,
                                       jpp_sdk_handle_t handle);
```
```python
jppsdk.handle_close(handle: int) -> None
```

**Notes:** All open handles are automatically closed when the app exits. Close handles promptly — the limit is 4 per session.

---

## Key-value store

A simple persistent key-value store backed by `.kv.json` in the app's scoped directory (`/sd/apps/<app_id>/.kv.json`). Survives app restarts; suitable for preferences, scores, cached data.

**Capability:** None

### `kv_get`

Return the value for a key, or `None`/`JPP_SDK_NO_DATA` if the key does not exist.

```c
jpp_sdk_status_t jpp_sdk_kv_get(jpp_sdk_context_t *ctx,
                                 const char *key,
                                 char *out_value,
                                 size_t value_buf_len);
```
```python
jppsdk.kv_get(key: str) -> str | None
```

**Parameters:**

| Name | Limit |
|------|-------|
| Key | 64 characters |
| Value | 256 characters |

**Returns:**
- C: `JPP_SDK_OK` if the key exists, `JPP_SDK_NO_DATA` if absent.
- Python: the value string, or `None` if the key does not exist (does not raise).

---

### `kv_set`

Set or update a key.

```c
jpp_sdk_status_t jpp_sdk_kv_set(jpp_sdk_context_t *ctx,
                                 const char *key,
                                 const char *value);
```
```python
jppsdk.kv_set(key: str, value: str) -> None
```

---

### `kv_delete`

Delete a key. No-op if the key does not exist.

```c
jpp_sdk_status_t jpp_sdk_kv_delete(jpp_sdk_context_t *ctx,
                                    const char *key);
```
```python
jppsdk.kv_delete(key: str) -> None
```

---

## IPC

Apps can send and receive text messages through file-backed mailboxes at `/sd/ipc/<recipient>/<sender>/`. Messages are consumed and deleted on receive.

**Capability:** None

### `ipc_send`

Send a message to another app's mailbox.

```c
jpp_sdk_status_t jpp_sdk_ipc_send(jpp_sdk_context_t *ctx,
                                   const char *recipient_app_id,
                                   const char *payload,
                                   jpp_broker_result_t *result);
```
```python
jppsdk.ipc_send(recipient_id: str, payload: str) -> None
```

**Parameters:**

| Name | Description |
|------|-------------|
| `recipient_app_id` | The `app_id` of the target app. The target does not need to be running. |
| `payload` | Message body. Maximum 512 bytes. |

---

### `ipc_recv`

Check this app's mailbox and return the oldest unread message. Non-blocking.

```c
jpp_sdk_status_t jpp_sdk_ipc_recv(jpp_sdk_context_t *ctx,
                                   char *out_payload,
                                   size_t payload_buf_len,
                                   jpp_broker_result_t *result);
```
```python
jppsdk.ipc_recv() -> tuple[str, str] | None
```

**Returns:**
- C: `JPP_SDK_OK` if a message was read; `JPP_SDK_NO_DATA` if the mailbox is empty. When OK, `result->text` is the payload and `result->sender` is the sender's `app_id`.
- Python: `(payload, sender_app_id)` tuple, or `None` if no messages.

**Notes:** Messages are deleted from the mailbox on receipt. Poll `ipc_recv()` in `on_idle` to process incoming messages.

---

## HTTP

**Capability:** `http.request` (Tier 1 — one-time grant, persisted)

### `http_request`

Make a synchronous HTTP GET or POST request.

```c
jpp_sdk_status_t jpp_sdk_http_request(jpp_sdk_context_t *ctx,
                                       const char *method,
                                       const char *url,
                                       const char *body,
                                       jpp_broker_result_t *result);
```
```python
jppsdk.http_request(method: str, url: str, body: str | None = None) -> dict
```

**Parameters:**

| Name | Description |
|------|-------------|
| `method` | `"GET"` or `"POST"`. |
| `url` | Full URL including scheme, e.g. `"http://192.168.1.1/api/data"`. |
| `body` | Request body for POST. Pass `NULL`/`None` for GET. |

**Returns:**
- C: `JPP_SDK_OK`; `result->status_code` is the HTTP status (e.g. `"200"`) and `result->body` is the response body.
- Python: `{"status_code": int, "body": str}`.

**Notes:**
- HTTP requests are serialized through the broker — only one can be in flight at a time.
- HTTPS is not supported in the current firmware.
- Wi-Fi must be connected; the request fails with a broker error if the network is unavailable.

---

## Network (TCP server)

**Capability:** `network.bind` (Tier 2 — per-session grant)

One listener socket, up to 2 accepted connections. Binding fails while the WebDAV or LRV server is running. All sockets close automatically when the app exits.

### `net_bind`

Open a TCP listener on the specified port.

```c
jpp_sdk_status_t jpp_sdk_net_bind(jpp_sdk_context_t *ctx,
                                   uint16_t port,
                                   jpp_broker_result_t *result);
```
```python
jppsdk.net_bind(port: int) -> None
```

**Returns:** `JPP_SDK_OK`; `result->port` is the bound port number (as a string).

---

### `net_accept`

Wait for an incoming connection. Returns a socket id, or `NULL`/`-1` on timeout.

```c
jpp_sdk_status_t jpp_sdk_net_accept(jpp_sdk_context_t *ctx,
                                     uint32_t timeout_ms,
                                     int *out_sock,
                                     jpp_broker_result_t *result);
```
```python
jppsdk.net_accept(timeout_ms: int) -> int | None
```

**Parameters:**

| Name | Description |
|------|-------------|
| `timeout_ms` | How long to wait for a connection. Pass `0` to wait indefinitely. |
| `out_sock` / return | Socket identifier for `net_recv`/`net_send`/`net_close`. `-1` if timed out. |

---

### `net_recv`

Receive up to `max_len` bytes from a connected socket.

```c
jpp_sdk_status_t jpp_sdk_net_recv(jpp_sdk_context_t *ctx,
                                   int sock,
                                   uint8_t *buf,
                                   size_t buf_len,
                                   size_t *out_len,
                                   uint32_t timeout_ms,
                                   jpp_broker_result_t *result);
```
```python
jppsdk.net_recv(sock: int, max_len: int, timeout_ms: int) -> bytes
```

**Parameters:**

| Name | Description |
|------|-------------|
| `sock` | Socket id from `net_accept`. |
| `buf_len` / `max_len` | Maximum bytes to read. Hard limit: 1024 bytes per call. |
| `timeout_ms` | Receive timeout. Pass `0` to wait indefinitely. |

**Returns:**
- C: `JPP_SDK_OK`; `*out_len` is bytes received (0 on timeout); `result->closed` is `"1"` if the peer closed the connection.
- Python: `bytes` — empty `b""` on timeout or peer close.

---

### `net_send`

Send bytes to a connected socket.

```c
jpp_sdk_status_t jpp_sdk_net_send(jpp_sdk_context_t *ctx,
                                   int sock,
                                   const uint8_t *data,
                                   size_t len,
                                   jpp_broker_result_t *result);
```
```python
jppsdk.net_send(sock: int, data: bytes) -> None
```

---

### `net_close`

Close a socket. Pass `sock = -1` to close the listener (without closing accepted connections).

```c
jpp_sdk_status_t jpp_sdk_net_close(jpp_sdk_context_t *ctx,
                                    int sock,
                                    jpp_broker_result_t *result);
```
```python
jppsdk.net_close(sock: int) -> None
```

**Notes:** Close accepted sockets when done — the connection table has only 2 slots. Close the listener (`sock=-1`) when you no longer want new connections.

---

## BLE scan

**Capability:** `ble.scan` (Tier 1 — one-time grant, persisted)

### `ble_scan`

Perform a passive BLE scan and collect advertisement packets.

```c
jpp_sdk_status_t jpp_sdk_ble_scan(jpp_sdk_context_t *ctx,
                                   uint32_t duration_ms,
                                   jpp_sdk_ble_scan_result_t *results,
                                   size_t max_results,
                                   size_t *out_count,
                                   jpp_broker_result_t *result);
```
```python
jppsdk.ble_scan(duration_ms: int) -> list[dict]
```

**Parameters:**

| Name | Description |
|------|-------------|
| `duration_ms` | How long to scan, in milliseconds. |
| `results` | C only: array to fill. |
| `max_results` | C only: capacity of `results`. Maximum 20. |
| `out_count` | C only: number of results written. |

**Returns:**
- C: `JPP_SDK_OK`; `*out_count` devices found; `results[i]` has `address`, `name`, `rssi`, `ad_data`, `ad_data_len`.
- Python: `list[dict]`, each with keys `address` (str `"AA:BB:CC:DD:EE:FF"`), `name` (str, empty if unnamed), `rssi` (int dBm), `ad_data` (bytes up to 31).

**C struct — `jpp_sdk_ble_scan_result_t`:**

```c
typedef struct {
    char    address[18];       /* "AA:BB:CC:DD:EE:FF\0" */
    char    name[32];          /* local name, or "" if none */
    int8_t  rssi;              /* signal strength in dBm */
    uint8_t ad_data[31];       /* raw advertisement payload */
    size_t  ad_data_len;       /* bytes used in ad_data */
} jpp_sdk_ble_scan_result_t;
```

---

## BLE advertise

**Capability:** `ble.advertise` (Tier 1 — one-time grant, persisted)

### `ble_advertise_start`

Begin broadcasting a BLE advertisement payload.

```c
jpp_sdk_status_t jpp_sdk_ble_advertise_start(jpp_sdk_context_t *ctx,
                                              const uint8_t *payload,
                                              size_t payload_len,
                                              jpp_broker_result_t *result);
```
```python
jppsdk.ble_advertise_start(payload: bytes) -> None
```

**Parameters:**

| Name | Description |
|------|-------------|
| `payload` | Raw advertisement payload. Maximum 31 bytes. |

---

### `ble_advertise_stop`

Stop broadcasting.

```c
jpp_sdk_status_t jpp_sdk_ble_advertise_stop(jpp_sdk_context_t *ctx,
                                             jpp_broker_result_t *result);
```
```python
jppsdk.ble_advertise_stop() -> None
```

---

### `ble_set_connectable`

Set whether inbound BLE connections are accepted while advertising.

**Capability:** `ble.advertise`

```c
jpp_sdk_status_t jpp_sdk_ble_set_connectable(jpp_sdk_context_t *ctx,
                                              bool connectable,
                                              jpp_broker_result_t *result);
```

**Notes:** Python binding not available — use `ble_host` for accepting inbound connections from Python.

---

## BLE GATT client

**Capability:** `ble.connect` (Tier 2 — per-session grant)

Establish an outbound connection to a BLE peripheral and read/write its characteristics. Maximum 2 simultaneous connections per session.

### `ble_connect`

Connect to a BLE peripheral by address.

```c
jpp_sdk_status_t jpp_sdk_ble_connect(jpp_sdk_context_t *ctx,
                                      const char *address,
                                      jpp_sdk_ble_conn_t *out_conn);
```
```python
jppsdk.ble_connect(address: str) -> int
```

**Parameters:**

| Name | Description |
|------|-------------|
| `address` | BLE address string in format `"AA:BB:CC:DD:EE:FF"`. |
| `out_conn` / return | Connection handle for subsequent `ble_read_char`/`ble_write_char`/`ble_disconnect` calls. |

---

### `ble_read_char`

Read a GATT characteristic value.

```c
jpp_sdk_status_t jpp_sdk_ble_read_char(jpp_sdk_context_t *ctx,
                                        jpp_sdk_ble_conn_t conn,
                                        const char *svc_uuid,
                                        const char *char_uuid,
                                        jpp_broker_result_t *result);
```
```python
jppsdk.ble_read_char(conn: int, svc_uuid: str, char_uuid: str) -> dict
```

**Parameters:**

| Name | Description |
|------|-------------|
| `svc_uuid` | Service UUID, full format `"XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"`. |
| `char_uuid` | Characteristic UUID, same format. |

**Returns:** `result->text` / `result["text"]` — the characteristic value as a string.

---

### `ble_write_char`

Write a value to a GATT characteristic.

```c
jpp_sdk_status_t jpp_sdk_ble_write_char(jpp_sdk_context_t *ctx,
                                         jpp_sdk_ble_conn_t conn,
                                         const char *svc_uuid,
                                         const char *char_uuid,
                                         const char *text,
                                         jpp_broker_result_t *result);
```
```python
jppsdk.ble_write_char(conn: int, svc_uuid: str, char_uuid: str, text: str) -> None
```

---

### `ble_disconnect`

Close a BLE connection.

```c
jpp_sdk_status_t jpp_sdk_ble_disconnect(jpp_sdk_context_t *ctx,
                                         jpp_sdk_ble_conn_t conn);
```
```python
jppsdk.ble_disconnect(conn: int) -> None
```

---

## BLE GATT server

**Capability:** `ble.host` (Tier 2 — per-session grant)

The firmware pre-registers a generic app-host GATT service. An app claims it with `ble_service_register`, publishes data via the TX characteristic, and receives data via the RX characteristic. There are no per-app GATT definitions in the firmware — apps identify their protocol through the advertisement payload.

**Fixed UUIDs:**

| Symbol | UUID | Direction |
|--------|------|-----------|
| `JPP_SDK_BLE_HOST_SVC_UUID` | `4a505300-0000-0000-0000-000000000000` | Service |
| `JPP_SDK_BLE_HOST_TX_UUID` | `4a505301-0000-0000-0000-000000000000` | Readable by peer |
| `JPP_SDK_BLE_HOST_RX_UUID` | `4a505302-0000-0000-0000-000000000000` | Writable by peer |

### `ble_service_register`

Claim the firmware's app-host GATT service for this session.

```c
jpp_sdk_status_t jpp_sdk_ble_service_register(jpp_sdk_context_t *ctx,
                                               const char *svc_uuid,
                                               jpp_broker_result_t *result);
```
```python
jppsdk.ble_service_register(svc_uuid: str) -> None
```

**Notes:** Pass `JPP_SDK_BLE_HOST_SVC_UUID` as the UUID. Only one app can claim the service at a time.

---

### `ble_service_unregister`

Release the GATT service.

```c
jpp_sdk_status_t jpp_sdk_ble_service_unregister(jpp_sdk_context_t *ctx,
                                                  jpp_broker_result_t *result);
```
```python
jppsdk.ble_service_unregister() -> None
```

---

### `ble_host_set_value`

Publish bytes to the TX characteristic so connected peers can read them.

```c
jpp_sdk_status_t jpp_sdk_ble_host_set_value(jpp_sdk_context_t *ctx,
                                              const uint8_t *data,
                                              size_t len,
                                              jpp_broker_result_t *result);
```

---

### `ble_host_wait_write`

Block until a peer writes to the RX characteristic, or the timeout elapses.

```c
jpp_sdk_status_t jpp_sdk_ble_host_wait_write(jpp_sdk_context_t *ctx,
                                              uint8_t *buf,
                                              size_t *len_inout,
                                              uint32_t timeout_ms,
                                              bool *out_received,
                                              jpp_broker_result_t *result);
```

**Parameters:**

| Name | Description |
|------|-------------|
| `buf` | Buffer to receive the written bytes. |
| `len_inout` | In: buffer capacity. Out: bytes received. |
| `timeout_ms` | Maximum wait time. |
| `out_received` | Set to `true` if a write was received within the timeout. |

---

### `ble_host_clear`

Clear the RX write buffer.

```c
jpp_sdk_status_t jpp_sdk_ble_host_clear(jpp_sdk_context_t *ctx,
                                         jpp_broker_result_t *result);
```

---

## Background registration

**Capability:** `background.register` (Tier 1 — one-time grant, persisted)

### `background_register`

Trigger the consent prompt for background task scheduling. The schedule itself is defined in `manifest.json`; this call only asks the user to approve it.

```c
jpp_sdk_status_t jpp_sdk_background_register(jpp_sdk_context_t *ctx,
                                              jpp_broker_result_t *result);
```
```python
jppsdk.background_register() -> None
```

**Notes:**
- Call this once at app startup if background tasks are important to the app's core function.
- If the user denies: `ACCESS_DENIED` / `SdkPermissionError`. The app keeps running without background tasks.
- If already granted (persisted from a prior launch): returns `OK` immediately without showing a prompt.
- After granting, the firmware syncs the app's background tasks into the scheduler every time the app exits. Tasks are removed from the scheduler if the manifest stops declaring them.

---

## Code modules (native apps only)

See [Native code modules](native/modules.md) for the full guide. Quick reference:

```c
/* Load a .mod.bin from the app's scoped directory into the pool tail */
jpp_sdk_status_t jpp_sdk_module_load(jpp_sdk_context_t *ctx,
                                      const char *relative_path,
                                      void **out_module);

/* Call jpp_module_entry(ctx, api) — blocks until the module returns */
jpp_sdk_status_t jpp_sdk_module_run(jpp_sdk_context_t *ctx,
                                     void *module,
                                     void *api);

/* Unload the module and free the pool tail */
jpp_sdk_status_t jpp_sdk_module_unload(jpp_sdk_context_t *ctx,
                                        void *module);
```

**Capability:** None — modules are loaded from the app's own scoped directory and run with the host app's capabilities.

---

## Resource limits

| Resource | Limit |
|----------|-------|
| Frame text rows | 7 |
| Frame row width | ~21 characters |
| Canvas (windowed) | 128×48 pixels |
| Canvas (fullscreen) | 128×64 pixels |
| Open file handles | 4 per session |
| KV key length | 64 characters |
| KV value length | 256 characters |
| IPC payload | 512 bytes |
| File path length | 160 characters |
| Input return value | 64 characters |
| BLE scan results | 20 |
| BLE advertisement payload | 31 bytes |
| BLE device name | 32 characters |
| BLE simultaneous connections | 2 per session |
| TCP listener | 1 |
| TCP accepted connections | 2 |
| TCP recv buffer | 1024 bytes per call |
| Background tasks (device-wide) | 8 |
| Background task run quota | 30 seconds |
| Background task minimum interval | 60 seconds |
| Manifest capabilities | 16 |
| Native app pool | 64 KB (hub + one module) |
