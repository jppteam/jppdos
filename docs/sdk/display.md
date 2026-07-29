# Canvas and UI helpers

Two ways to put something on the 128×64 OLED: the **canvas** for pixels you draw
yourself, and the **UI helpers** for the stock modal widgets that match the rest
of the device. Both are ungated — no capability, no prompt — except
[`file_pick`](#file_pick), which browses the whole card.

## Canvas

The canvas is a **128×48 pixel area** occupying OLED pages 2–7 (below the frame text). In fullscreen mode it expands to **128×64 pixels** covering the entire display.

Each pixel row is 16 bytes (128 bits). In each byte, the most significant bit is the leftmost pixel: byte 0 bits `[7..0]` map to pixels x=0..7, byte 1 to x=8..15, and so on.

!!! warning "`set_frame` and every modal drop fullscreen mode."
    [`canvas_fullscreen(true)`](#canvas_fullscreen) is cleared by
    [`set_frame`](app-control.md#set_frame) and by `dialog` / `list` / `input` /
    `confirm`. Re-enable it after any of those calls, or your next frame draws
    into the 48-row window.

### `canvas_write`

Write a complete row of pixels to the canvas.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_canvas_write(jpp_sdk_context_t *ctx,
                                       uint8_t row,
                                       const uint8_t *pixels);
```
///

/// tab | MicroPython
```python
jppsdk.canvas_write(row: int, pixels: bytes) -> None
```
///

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

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_canvas_draw_pixel(jpp_sdk_context_t *ctx,
                                            uint8_t x, uint8_t y,
                                            bool on);
```
///

/// tab | MicroPython
```python
jppsdk.canvas_draw_pixel(x: int, y: int, on: bool) -> None
```
///

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

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_canvas_clear(jpp_sdk_context_t *ctx);
```
///

/// tab | MicroPython
```python
jppsdk.canvas_clear() -> None
```
///

---

### `canvas_fullscreen`

Toggle the fullscreen canvas mode.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_canvas_fullscreen(jpp_sdk_context_t *ctx, bool on);
```
///

/// tab | MicroPython
```python
jppsdk.canvas_fullscreen(on: bool) -> None
```
///

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

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_dialog(jpp_sdk_context_t *ctx,
                                 const char *title,
                                 const char *text,
                                 jpp_sdk_ui_result_t *out_result);
```
///

/// tab | MicroPython
```python
jppsdk.dialog(text: str, title: str = None) -> bool
```
///

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

/// tab | C
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
///

/// tab | MicroPython
```python
jppsdk.list(items: list[str],
            title: str = None,
            multiselect: bool = False) -> int | list[int] | None
```
///

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

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_input(jpp_sdk_context_t *ctx,
                                const char *title,
                                const char *placeholder,
                                jpp_sdk_input_type_t type,
                                char *out_value,
                                size_t value_buf_len,
                                jpp_sdk_ui_result_t *out_result);
```
///

/// tab | MicroPython
```python
jppsdk.input(title: str,
             placeholder: str = None,
             type: int = jppsdk.INPUT_TEXT) -> str | None
```
///

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

### `confirm` (C only) { #confirm }

Show a customizable Deny/Allow prompt. This is the same consent surface used by capability and `files.full` path prompts.

**Capability:** None

!!! info "Resolvable from SDK level 2 onwards."
    `jpp_sdk_confirm` was declared in the v1.0-RTM header but was missing from
    the firmware's native symbol table, so a native app that called it failed to
    launch with `UNRESOLVED_SYM`. It became callable in firmware v1.1 — declare
    `"sdk_min": 2` if you use it. See the [SDK changelog](../sdk-changelog.md).

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

### `file_pick` (C only) { #file_pick }

Launch a full SD card file browser and return the path the user selects.

!!! warning "Capability: `files.full`."
    **Tier 2** — prompted on first use of every launch, never persisted. See
    [Full file access](storage.md#full-file-access).

```c
jpp_sdk_status_t jpp_sdk_file_pick(jpp_sdk_context_t *ctx,
                                    char *out_path,
                                    size_t out_path_len,
                                    jpp_sdk_ui_result_t *out_result);
```

**Notes:** The browser starts at the SD root (`/sd`). Directories appear with a `/` suffix; `..` navigates up. Long names scroll as marquees. `out_path` receives the absolute path of the selected file. Returns `JPP_SDK_UI_BACK` if the user cancels without selecting a file.

There is no direct Python equivalent; use `file_open` with a known path for MicroPython apps that need full access.

---

### `wrap_text` (C only) { #wrap_text }

Word-wrap a string into fixed-width frame lines.

```c
size_t jpp_sdk_wrap_text(const char *text,
                          char lines[][JPP_SDK_FRAME_TEXT_MAX],
                          size_t max_lines);
```

**Returns:** Number of lines produced. Use this to split a long message before passing it to `set_frame` or a modal helper.

!!! danger "Not callable from a loaded app binary."
    `jpp_sdk_wrap_text` is compiled into the firmware but is **absent from the
    native symbol table**, so a native app that calls it is rejected at launch
    with `UNRESOLVED_SYM`. Until that is fixed, wrap text in your own code.
    (Firmware-side: add it to `s_symtab` in
    `components/jpp_native_loader_core/src/jpp_native_symtab.c`.)

---
