# App control and input

The calls every app touches: putting text on screen, reading the d-pad, writing
to the log, and shutting down. All ungated — no capability, no prompt — except
[`request_cap`](#request_cap), which exists precisely to trigger one.

## App control

### `set_frame`

Sets the text content displayed on the OLED above the canvas area.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_set_frame(jpp_sdk_context_t *ctx,
                                    const char *const *lines,
                                    size_t line_count);
```
///

/// tab | MicroPython
```python
jppsdk.set_frame(lines: list[str]) -> None
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `lines` | Array of text rows. Each row fits ~21 characters; longer strings are silently truncated. |
| `line_count` | Number of rows. Maximum 7. Extra rows beyond the limit are dropped. |

**Returns:** `JPP_SDK_OK`, or `JPP_SDK_TEXT_TRUNCATED` if any line was clipped.

**Notes:**
- Calling `set_frame` clears fullscreen canvas mode, taking the app back to the 48-row window for the duration. When the modal returns, the firmware restores fullscreen if the app was in fullscreen when the frame was shown, so apps do not need to re-enable it after every prompt.
- Row 0 acts as the title — if non-empty, a 1-px signature rule is drawn below it.
- The frame persists until the next `set_frame`. You do not need to call it on every idle tick; call it only when content changes.

---

### `request_close`

Signals the firmware to close the app and return to the launcher.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_request_close(jpp_sdk_context_t *ctx);
```
///

/// tab | MicroPython
```python
jppsdk.request_close() -> None
```
///

**Notes:**
- For MicroPython apps, the firmware calls `on_stop` and then tears down the runtime. You can still do cleanup after `request_close()` returns — the app is not immediately killed.
- For native apps, call `request_close` then return from `jpp_app_entry`. The firmware closes the app when the function returns; you can also return without calling it and the firmware will close cleanly.
- Calling `request_close` inside `on_idle` sets a flag; the app runs until `on_stop` completes.

---

### `log`

Emits a named event to the device log (visible over serial monitor).

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_log(jpp_sdk_context_t *ctx,
                              const char *event_name);
```
///

/// tab | MicroPython
```python
jppsdk.log(event_name: str) -> None
```
///

**Notes:** Useful for debugging. Log lines appear on the device console (native USB-Serial-JTAG) with the tag `app_log`.

---

### `request_cap` (C only) { #request_cap }

Proactively triggers the consent prompt for a single manifest-declared capability, without performing any operation. Use it to **front-load** permission requests — ask for the caps a screen or mode needs the moment the user chooses it, rather than letting the prompt fire mid-flow at first use.

**Capability:** the one named by `cap` (must be declared in the manifest)

```c
jpp_sdk_status_t jpp_sdk_request_cap(jpp_sdk_context_t *ctx,
                                     const char *cap);
```

**Returns:** `JPP_SDK_OK` if the cap is already granted or the user allows it; `JPP_SDK_ACCESS_DENIED` if the user declines or the cap was not declared in the manifest; `JPP_SDK_INVALID_ARGUMENT` if `cap` is `NULL`/empty.

**Notes:**
- This only changes *when* the prompt appears, never the policy. Tier-1 caps (e.g. `ble.scan`, `ble.advertise`, `http.request`, `background.register`) persist once granted; tier-2 caps (e.g. `files.full`, `ble.connect`, `ble.host`, `network.bind`) are granted for the session only and re-prompt on the next launch — identical to first-use consent.
- Requesting an already-granted cap is a cheap no-op that returns `JPP_SDK_OK` without prompting, so it is safe to call on every entry to a screen.
- `https.request` is a partial exception: requesting it front-loads the capability prompt, but the [per-origin prompt](network.md#per-origin-consent) still fires on the first request to each new host, because the origin is not known until the call.
- During a headless background run every request is denied (`JPP_SDK_ACCESS_DENIED`), matching the first-use rule.
- MeetApp is the reference user: it requests `ble.scan`/`ble.advertise` at startup and the mode-specific `ble.connect`/`ble.host` the moment the user picks "Initiate" or "Join".

---

## Key input

### `poll_key`

Returns the next key event from the queue without blocking. Returns `KEY_NONE` immediately if no key is pending.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_poll_key(jpp_sdk_context_t *ctx,
                                   jpp_sdk_key_event_t *out_event);
```
///

/// tab | MicroPython
```python
jppsdk.poll_key() -> int
```
///

**Returns:**
- C: `JPP_SDK_OK`; `*out_event` is the key constant (may be `JPP_SDK_KEY_NONE`)
- Python: the key constant as an `int`

**Notes:** Use this in `on_idle` where you want to check for keys but not block the idle loop.

---

### `wait_key`

Blocks until a key event is available or the timeout elapses.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_wait_key(jpp_sdk_context_t *ctx,
                                   uint32_t timeout_ms,
                                   jpp_sdk_key_event_t *out_event);
```
///

/// tab | MicroPython
```python
jppsdk.wait_key(timeout_ms: int) -> int
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `timeout_ms` | Maximum wait time in milliseconds. Pass `0` to wait indefinitely. |

**Returns:** The key constant. `JPP_SDK_KEY_NONE` if the timeout elapsed with no key.

**Notes:** Use this as the main loop driver in native apps or in MicroPython blocking sequences. Pass a short timeout (e.g. 100 ms) when you need periodic work between key events.

---

### `push_key` (C only) { #push_key }

Injects a synthetic key event into the queue. Useful for testing and for UI helpers that need to replay a key.

**Capability:** None

```c
void jpp_sdk_push_key(jpp_sdk_context_t *ctx, jpp_sdk_key_event_t event);
```

---

### `claim_center`

Take over CENTER gestures as your own input.

**Capability:** None

!!! info "Requires SDK level 2."
    `claim_center`, `KEY_BACK`, `KEY_CENTER_HOLD`, `KEY_CENTER_DOUBLE` and the
    `CENTER_CLAIM_*` constants all arrived in firmware v1.1. Declare
    `"sdk_min": 2` — see the [SDK changelog](../sdk-changelog.md).

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_claim_center(jpp_sdk_context_t *ctx, uint8_t mask);
```
///

/// tab | MicroPython
```python
jppsdk.claim_center(mask: int) -> None
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `mask` | Bitwise OR of `JPP_SDK_CENTER_CLAIM_HOLD` and `JPP_SDK_CENTER_CLAIM_DOUBLE`, or `JPP_SDK_CENTER_CLAIM_NONE` (the default on every bind). In MicroPython: `jppsdk.CENTER_CLAIM_HOLD`, `jppsdk.CENTER_CLAIM_DOUBLE`, `jppsdk.CENTER_CLAIM_NONE`. |

**Behaviour:**

| Claim | Your app receives | Back |
|---|---|---|
| *(nothing — the default)* | `KEY_CENTER` | `KEY_BACK` |
| `HOLD` | `KEY_CENTER` + `KEY_CENTER_HOLD` | your own |
| `DOUBLE` | `KEY_CENTER` + `KEY_CENTER_DOUBLE` | your own |
| `HOLD \| DOUBLE` | `KEY_CENTER` + both | your own |

**Notes:** The device has a user preference (Settings > Controls) for whether a long hold or a double-click means "Back". **Your app never needs to read it.** Claim nothing and you get `JPP_SDK_KEY_BACK` whenever the user asks to go back, with the firmware deciding which physical gesture that was — settings-agnostic by construction.

Claim a gesture and it becomes yours: it arrives as `JPP_SDK_KEY_CENTER_HOLD` / `JPP_SDK_KEY_CENTER_DOUBLE`, `JPP_SDK_KEY_BACK` stops being delivered, and **your app is responsible for its own way out** (a pause menu, an on-screen Exit item). That is the trade for owning the gesture, and it applies whichever gesture you claimed.

Claiming *only* `HOLD` additionally keeps `JPP_SDK_KEY_CENTER` instant. Telling a double-click apart requires withholding the first click for a few hundred milliseconds; when nobody needs that distinction, nothing is withheld. This is the combination for an app where CENTER is a rapid action button and hold opens a pause menu:

```c
jpp_sdk_claim_center(ctx, JPP_SDK_CENTER_CLAIM_HOLD);
/* ... */
switch (key) {
case JPP_SDK_KEY_CENTER:      fire();       break;
case JPP_SDK_KEY_CENTER_HOLD: pause_menu(); break;
}
```

Never affects `UP`/`DOWN`/`LEFT`/`RIGHT`, and never affects the launcher or Settings. The claim lives on your context and is dropped when your app exits.

`JPP_SDK_KEY_BACK` is the preferred spelling of `JPP_SDK_KEY_CENTER_LONG` — the same value under a name that no longer implies a particular gesture. Existing code using `JPP_SDK_KEY_CENTER_LONG` is unaffected.

---
