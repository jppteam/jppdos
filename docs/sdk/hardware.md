# Buzzer, LED, and device status

The piezo buzzer, the onboard WS2812 pixel, the wakelock, and the read-only
device state (battery, clock, username, dummy mode). Everything on this page is
ungated: no manifest declaration, no consent prompt.

## Buzzer

### `buzzer_play`

Play a predefined sound effect.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_buzzer_play(jpp_sdk_context_t *ctx,
                                      jpp_buzzer_sound_t sound);
```
///

/// tab | MicroPython
```python
jppsdk.buzzer_play(sound: int) -> None
```
///

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

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_buzzer_tone(jpp_sdk_context_t *ctx,
                                      uint32_t freq_hz,
                                      uint32_t duration_ms);
```
///

/// tab | MicroPython
```python
jppsdk.buzzer_tone(freq_hz: int, duration_ms: int) -> None
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `freq_hz` | Tone frequency in Hz. Pass `0` for silence (a rest/pause). |
| `duration_ms` | Duration in milliseconds. |

---

### `buzzer_play_sequence`

Play a sequence of notes. Blocks until the entire sequence finishes.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_buzzer_play_sequence(jpp_sdk_context_t *ctx,
                                               const jpp_buzzer_note_t *notes,
                                               size_t count);
```
///

/// tab | MicroPython
```python
jppsdk.buzzer_play_sequence(notes: list[tuple[int, int]]) -> None
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `notes` | Array of `(freq_hz, duration_ms)` pairs. `freq_hz=0` is a rest. |
| `count` | Number of notes (C only). |

**Notes:** Preempts any currently playing async sequence.

**Example:**

/// tab | C
```c
jpp_buzzer_note_t melody[] = {
    { 440, 150 }, { 0, 50 }, { 523, 150 }, { 659, 300 }
};
jpp_sdk_buzzer_play_sequence(ctx, melody, 4);
```
///

/// tab | MicroPython
```python
jppsdk.buzzer_play_sequence([(440, 150), (0, 50), (523, 150), (659, 300)])
```
///

---

### `buzzer_play_sequence_async`

Play a note sequence without blocking the caller. Returns immediately; the sequence plays in a dedicated task.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_buzzer_play_sequence_async(jpp_sdk_context_t *ctx,
                                                     const jpp_buzzer_note_t *notes,
                                                     size_t count);
```
///

/// tab | MicroPython
```python
jppsdk.buzzer_play_sequence_async(notes: list[tuple[int, int]]) -> None
```
///

**Notes:**
- The sequence is **copied** before the call returns — you do not need to keep `notes` alive.
- Starting a new async sequence (or calling `buzzer_stop`) preempts the previous one within one note period. This makes it suitable for cycling through jingle previews.
- `buzzer_play_sequence` (blocking) also preempts any running async sequence.

---

### `buzzer_stop`

Stop any currently playing sound immediately.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_buzzer_stop(jpp_sdk_context_t *ctx);
```
///

/// tab | MicroPython
```python
jppsdk.buzzer_stop() -> None
```
///

---

## LED

Onboard WS2812 addressable RGB pixel.

### `led_set_color`

Set the onboard LED to an RGB color.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_led_set_color(jpp_sdk_context_t *ctx,
                                        uint8_t r, uint8_t g, uint8_t b);
```
///

/// tab | MicroPython
```python
jppsdk.led_set_color(r: int, g: int, b: int) -> None
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `r`, `g`, `b` | Color channels, 0-255 each. |

---

### `led_off`

Turn the onboard LED off. Equivalent to `led_set_color(0, 0, 0)`.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_led_off(jpp_sdk_context_t *ctx);
```
///

/// tab | MicroPython
```python
jppsdk.led_off() -> None
```
///

---

## Wakelock

The wakelock prevents the device from dimming the OLED or entering deep sleep while your app is active. It does not prevent the user from forcing sleep via the power button.

### `wakelock_acquire`

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_wakelock_acquire(jpp_sdk_context_t *ctx);
```
///

/// tab | MicroPython
```python
jppsdk.wakelock_acquire() -> None
```
///

Acquire the wakelock. The device will not dim or sleep while the lock is held.

---

### `wakelock_release`

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_wakelock_release(jpp_sdk_context_t *ctx);
```
///

/// tab | MicroPython
```python
jppsdk.wakelock_release() -> None
```
///

Release the wakelock. The OS resumes normal power management.

**Notes:** The wakelock is automatically released when the app exits. Acquire it for as short a time as possible — holding it indefinitely drains the battery.

---

## Device status

### `device_status`

Return the current battery/charging state and the device's username.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_device_status(jpp_sdk_context_t *ctx,
                                        jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.device_status() -> dict
```
///

**Returns:**
- C: `JPP_SDK_OK`; `result` fields `battery_pct` (integer as string, `-1` if unknown), `charging` (`"1"` if charging, `"0"` if not, `"-1"` if unknown), and `username` (the device's stored user name, `Settings > User's name` — empty string if unset).
- Python: `{"battery_pct": int, "charging": str, "username": str}`.

---

### `get_time`

Return the current date and time from the RTC.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_get_time(jpp_sdk_context_t *ctx,
                                   jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.get_time() -> str
```
///

**Returns:**
- C: `JPP_SDK_OK`; `result->text` is `"YYYY-MM-DD HH:mm"`.
- Python: `"YYYY-MM-DD HH:mm"` as a string.

**Notes:** Reads the time from the RTC/software clock. The DS1307 hardware clock is optional: if no RTC chip is fitted and the time has not been set (e.g. via NTP), the call fails (C: non-`OK` status with `result->text` = `"RTC_UNAVAILABLE"`; Python: raises) — your app should handle this and display `--:--` rather than a bogus value. When the time is available it is in the device's configured timezone.

---

### `is_dummy_mode`

Return whether the firmware has locked the device to this app (dummy mode).

**Capability:** None

/// tab | C
```c
bool jpp_sdk_is_dummy_mode(const jpp_sdk_context_t *ctx);
```
///

/// tab | MicroPython
```python
jppsdk.is_dummy_mode() -> bool
```
///

**Returns:**
- C: `true` if the app is the dummy-mode locked app, `false` otherwise (including if `ctx` is `NULL`).
- Python: `True` or `False`.

**Notes:** When dummy mode is active the firmware re-launches this app automatically after it exits and blocks all launcher navigation. Apps can call this to detect the condition and hide their own "Exit" option, since the firmware will re-launch them regardless. Dummy mode is enabled in **Settings → Dummy Mode** and disabled by holding CENTER on boot.

---
