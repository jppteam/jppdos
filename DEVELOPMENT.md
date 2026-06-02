# App development guide

This guide is for **writing apps for the J++Device**. You don't need to know
ESP-IDF or the firmware internals to build an app — you write your code against
the **App SDK**, package it with a small `manifest.json`, and drop it on the SD
card. The device discovers it, shows it in the launcher, and runs it in a
sandbox.

## What an app is

Apps come in two flavours:

- **MicroPython** (recommended) — write Python, compile it to a `.mpy` file, call
  the `jppsdk` module, and drop the folder on the SD card under
  `/sd/apps/<app_id>/` with a `manifest.json`. Easiest way to start.
- **Native C** — write C against the `jpp_sdk_*` API, build it to a `.bin`, and
  package it the same way (`/sd/apps/<app_id>/` + `manifest.json`, with
  `app_type: "native"`). Use this when you need direct, low-latency hardware
  work. There is no special "system" status — a native app runs under the same
  capability/consent flow as any other app. (See the native loader note under
  [Application model](#application-model) for how native binaries are currently
  delivered while the dynamic SD loader is pending.)

Whatever the language, apps never touch the screen, files, network, or radio
directly. Everything goes through the **App SDK**, and anything sensitive is
gated by a capability the user grants. This keeps apps safe and portable.

## How apps fit into the device

The firmware (the part you do *not* write) owns boot, storage, settings, the
service broker, drivers, and the UI. Your app talks to it through the App SDK:

```
your app  ──>  App SDK  ──>  service broker  ──>  hardware / storage / radio
```

The App SDK is exposed two ways from the same underlying implementation:

- to MicroPython apps as the importable `jppsdk` module, and
- to native C apps as the `jpp_sdk_*` functions in `jpp_sdk_bridge.h`.

If you want to read the firmware itself, the implementation lives under
`components/jpp_core/` (`jpp_sdk_bridge` is the App SDK, `jpp_broker_core` is the
capability gate, `jpp_ui_core` is the display/launcher). You only need that to
modify the firmware, not to write apps.

## Building the firmware (only if you change the firmware)

Use the Docker-based ESP-IDF flow from the repository root:

```bash
docker compose run --rm build idf.py build
docker compose run --rm build idf.py menuconfig
docker compose run --rm build idf.py fullclean
```

For flashing and serial monitoring on the default host port:

```bash
docker compose run --rm build idf.py -p /dev/ttyUSB0 flash monitor
```

These are the supported development commands for this repo. Keep command usage aligned with standard ESP-IDF practice.

## Runtime architecture

The firmware is organized around a native app/runtime boundary:

- the core owns boot, persistence, broker policy, and hardware access
- apps do not talk to drivers directly
- privileged operations are mediated by the broker and the App SDK
- runtime decisions are enforced with error codes and policy checks instead of implicit access

This native architecture is the basis for app loading, recovery, and UI supervision.

## Application model

Every app — regardless of how it is packaged — runs the same way: on its own
task, with a bound App SDK context, under capabilities the user granted. There
are no privileged "system" apps; the only special screens are firmware UI
(Settings, Files, About). An app package lives under `/sd/apps/<app_id>/` with a
`manifest.json`, and the `app_type` field selects how it is built and run:

- **MicroPython apps** — Python compiled to `.mpy` bytecode, run by the on-device
  interpreter.
- **Native binary apps** — C compiled to a `.bin`, run as native code. The right
  choice for apps that need direct, low-latency hardware work (MeetApp, for
  example).

Both are external, manifest-described app packages — not firmware. The launcher
and the launch/teardown lifecycle treat every app the same way: the app's id is
pushed as the active screen, the main loop starts its task, and the app's SDK
frame is drawn on the OLED. The app boundary is enforced by the capability model
and the broker for every app, native or MicroPython.

### App types

The `app_type` field in a manifest selects the runtime:

| `app_type` | Entry | Execution |
|---|---|---|
| `"micropython"` (default) | `.mpy` compiled bytecode | MicroPython 1.28.0 interpreter |
| `"native"` | `.bin` ELF32 shared object | Native C, loaded from the SD card by `jpp_native_loader_core` at launch and freed when the app exits. |

Both types load and run directly from `/sd/apps`. The launcher, lifecycle, and
capability model treat them identically.

### Native app binary format

A native app binary is an **ELF32 RISC-V shared object** (`ET_DYN`) compiled
with `-fPIC -shared -mno-relax -nostartfiles` targeting `riscv32-esp-elf`. It
must export one global function:

```c
void jpp_app_entry(jpp_sdk_context_t *ctx);
```

All calls to firmware functions (`jpp_sdk_*`, `esp_log_write`, libc, etc.) are
left as undefined symbols in the binary; the loader resolves them at runtime from
the firmware's exported symbol table (`jpp_native_symtab.c`).

To build a native app (from the project root inside the Docker container):

```bash
idf.py build         # produces build/apps/<app_id>/<app_id>.bin alongside firmware
```

The `meetapp_bin` custom target in `apps/meetapp/CMakeLists.txt` calls
`apps/meetapp/build_shared.py`, which reads `build/compile_commands.json` for
the correct include paths and recompiles MeetApp as a shared library. The
resulting `build/apps/meetapp/meetapp.bin` (an ELF32 shared object — the `.bin`
extension is what the firmware expects) and `manifest.json` are dropped directly
into `build/apps/meetapp/` ready to copy to the SD card.

The `testapp_native_bin` custom target in `apps/testapp_native/CMakeLists.txt`
works identically, producing `build/apps/testapp_native/testapp_native.bin`.

The `testapp_mp_bin` custom target in `apps/testapp_mp/CMakeLists.txt` compiles
`main.py` to `main.mpy` with mpy-cross and stages both `main.mpy` and
`manifest.json` to `build/apps/testapp_mp/`. Requires `mpy-cross` 1.28.0 on
PATH (`pip install mpy-cross==1.28.0`). The target runs in parallel with the
firmware build since it has no dependency on `compile_commands.json`.

Every native build script now also copies `manifest.json` alongside the `.so` so
`build/apps/<app_id>/` is a self-contained deployment bundle.

### Reference / test apps

| App | Type | Purpose |
|---|---|---|
| `apps/meetapp/` | native C | BLE meetup proof — full example of multi-capability native app |
| `apps/BounceJPP/` | MicroPython | Minimal animation example (canvas + wakelock) |
| `apps/testapp_native/` | native C | SDK test app — exercises every `jpp_sdk_*` capability via a menu |
| `apps/testapp_mp/` | MicroPython | SDK test app — exercises every `jppsdk` Python function via a menu |

### Required app layout

Every app package is a directory under `/sd/apps/<app_id>/` containing a
`manifest.json` plus its entry file. A MicroPython app:

```text
/sd/apps/demo_clock/
├── manifest.json        # app_type: "micropython"
└── main.mpy
```

A native binary app uses the same shape, with a `.bin` ELF:

```text
/sd/apps/meetapp/
├── manifest.json        # app_type: "native", entry: "meetapp.bin"
└── meetapp.bin          # ELF32 RISC-V shared object, exports jpp_app_entry
```

The manifest records the app id, name, version, SDK range, entry path, capabilities, background policy, and package metadata. The loader rejects malformed or incompatible packages.

### Manifest schema v2

Schema v2 is the production app contract. Required fields include:

- `schema_version`: must be `2`
- `app_id`: stable unique identifier
- `name`: launcher label
- `version`: package version string
- `sdk_min` / `sdk_max`: inclusive SDK range
- `entry`: relative entry path (`.mpy` for MicroPython, `.bin` for native)
- `app_type`: `"micropython"` (default) or `"native"`
- `capabilities`: approved capability list (see below)
- `background.enabled`: background opt-in
- `background.mode`: must be `serialized`
- `toolchain.runtime_version`: MicroPython version — must be `"v1.28.0"` (MicroPython apps only)
- `toolchain.cross_version`: mpy-cross version — must be `"1.28.0"` (MicroPython apps only)
- `toolchain.bytecode_abi`: bytecode ABI version — must be `6` (MicroPython apps only)

The capability model remains broker-enforced. Keep capability names and access checks consistent with the native core.

### Capabilities and permission tiers

Capabilities are declared in the manifest and enforced by `jpp_broker_core`. There are three tiers:

**Tier 0 — auto-granted at load time**

| Capability | What it unlocks | Storage root |
|---|---|---|
| `files.scoped` | Read, write, and list the app's private SD directory | `/sd/apps/<app_id>/` |
| `files.shared` | Read, write, and list the app's subdirectory in the shared SD area | `/sd/shared/<app_id>/` |
| `device.status` | Single call returning time, network state, battery, and SD presence | — |

**Tier 1 — one-time user grant, persisted in `/data/grants/<app_id>.json`**

| Capability | What it unlocks |
|---|---|
| `ipc.send` | Post and receive small messages to/from other installed apps via file-backed mailbox |
| `http.request` | Make broker-serialized HTTP GET and POST requests |
| `device.kv` | Per-app persistent key-value store backed by `/data/kv/<app_id>.json` |
| `ble.scan` | Passive BLE scan — discover nearby devices, read advertisement payloads and RSSI |
| `ble.advertise` | Broadcast a raw BLE advertisement payload; broker serializes the radio slot |

**Tier 2 — per-session user grant, prompted at each app launch**

| Capability | What it unlocks |
|---|---|
| `files.full` | Full SD card access via user-approved path handles (`sdk_file_open`). Each path is individually approved; the handle persists for the session. |
| `network.bind` | Create TCP servers, HTTP servers, and UDP sockets |
| `ble.connect` | GATT client: connect to a BLE peripheral, read/write characteristics. Session-scoped connection handles (max `JPP_SDK_BLE_CONN_CAPACITY`). |
| `ble.host` | GATT server: register one service and accept inbound BLE connections. One registration per app session. |

Apps declaring Tier 1 or Tier 2 capabilities must also pass manifest validation; `jpp_manifest_v2_is_allowed_capability` is the authoritative whitelist.

#### How the user grants permissions

When an app launches, the firmware asks the user to approve the capabilities it
declares, before the app runs:

- **Tier 0** is granted automatically — no prompt.
- **Tier 1** shows an Allow/Deny screen the first time the app requests it. If
  allowed, the grant is saved to `/data/grants/<app_id>.json` and the app is not
  asked again on later launches. If denied, the app launches without that
  capability.
- **Tier 2** shows an Allow/Deny screen on every launch; nothing is persisted.

The prompt is written for the person holding the device: it shows a plain-language
sentence describing what the app wants to do (e.g. "This app wants to connect to
Bluetooth devices.") rather than the raw capability slug, and it does not repeat
the app name — the app is implied by what is on screen. **Deny** and **Allow** sit
at opposite edges of the bottom row. Capability prompts and `files.full` path
prompts share one consent surface (`jpp_sdk_confirm`), so every permission
decision — one-time or persisted — looks and behaves identically: a titled screen
with the signature line, the request body, and the Deny/Allow selector.

A denied capability is simply dropped: the app still launches, and any SDK call
that needs the missing capability returns `ACCESS_DENIED`. A well-behaved app
checks for this and tells the user (MeetApp, for instance, shows a "Permission
needed" message and returns to its menu rather than failing silently). On the
keypad, the prompt focuses **Deny** by default — d-pad LEFT/RIGHT picks
Deny/Allow, CENTER confirms, and a long-press CENTER (BACK) denies.

For `files.full`, approval is finer-grained: each individual path the app opens
with `file_open` triggers its own Allow/Deny dialog, and the resulting handle
stays valid for the session.

### App SDK surface (native C)

The App SDK calls in `jpp_sdk_bridge.h` are:

| Call | Requires |
|---|---|
| `jpp_sdk_set_frame`, `jpp_sdk_request_close`, `jpp_sdk_log` | — (always available) |
| `jpp_sdk_dialog`, `jpp_sdk_list`, `jpp_sdk_confirm`, `jpp_sdk_input` | — (high-level UI helpers) |
| `jpp_sdk_poll_key`, `jpp_sdk_wait_key` | — (always available) |
| `jpp_sdk_canvas_write`, `jpp_sdk_canvas_clear`, `jpp_sdk_canvas_draw_pixel` | — (built-in, no capability) |
| `jpp_sdk_wakelock_acquire`, `jpp_sdk_wakelock_release` | — (prevents screen dim and deep sleep) |
| `jpp_sdk_buzzer_play`, `jpp_sdk_buzzer_tone`, `jpp_sdk_buzzer_play_sequence`, `jpp_sdk_buzzer_stop` | — (no capability) |
| `jpp_sdk_device_status`, `jpp_sdk_get_time` | `device.status` |
| `jpp_sdk_file_read`, `jpp_sdk_file_write`, `jpp_sdk_file_list` | `files.scoped` |
| `jpp_sdk_shared_read`, `jpp_sdk_shared_write`, `jpp_sdk_shared_list` | `files.shared` |
| `jpp_sdk_file_open`, `jpp_sdk_handle_read`, `jpp_sdk_handle_write`, `jpp_sdk_handle_list`, `jpp_sdk_handle_close` | `files.full` |
| `jpp_sdk_add_background_task` | `background.register` (system-granted) |
| `jpp_sdk_http_request` | `http.request` |
| `jpp_sdk_ipc_send`, `jpp_sdk_ipc_recv` | `ipc.send` |
| `jpp_sdk_kv_get`, `jpp_sdk_kv_set`, `jpp_sdk_kv_delete` | `device.kv` |
| `jpp_sdk_ble_scan` | `ble.scan` |
| `jpp_sdk_ble_advertise_start`, `jpp_sdk_ble_advertise_stop` | `ble.advertise` |
| `jpp_sdk_ble_connect`, `jpp_sdk_ble_read_char`, `jpp_sdk_ble_write_char`, `jpp_sdk_ble_disconnect` | `ble.connect` |
| `jpp_sdk_ble_service_register`, `jpp_sdk_ble_service_unregister`, `jpp_sdk_ble_host_set_value`, `jpp_sdk_ble_host_wait_write`, `jpp_sdk_ble_host_clear` | `ble.host` |
| `jpp_sdk_ble_set_connectable` | `ble.advertise` |

The `ble.host` calls drive a single generic GATT *app-host* service that the
firmware pre-registers (NimBLE requires services to exist before the host
starts). An app claims it with `jpp_sdk_ble_service_register(JPP_SDK_BLE_HOST_SVC_UUID)`,
publishes bytes a connected peer can read with `jpp_sdk_ble_host_set_value`
(readable TX characteristic, `JPP_SDK_BLE_HOST_TX_UUID`), and receives bytes a
peer writes with `jpp_sdk_ble_host_wait_write` (writable RX characteristic,
`JPP_SDK_BLE_HOST_RX_UUID`). The service carries no app-specific protocol — apps
identify their own protocol through their advertisement payload — so there are no
per-app GATT definitions in the firmware.

`files.full` path approval is handled by the `path_prompt` callback in `jpp_sdk_native_services_t`. The firmware implements it with the shared `jpp_sdk_confirm` consent surface — the same Deny/Allow screen used for capability prompts — showing the requested path and access mode; the app's `file_open` call blocks until the user decides.

### App lifecycle

**MicroPython apps** — the runtime calls lifecycle hooks on the object returned by `create_app`:

- `on_start` — called once at launch
- `on_foreground` — called when the app gains the screen
- `on_background` — called when another screen is pushed on top
- `on_stop` — called at teardown
- `on_idle` — called approximately every 100 ms while the app is in the foreground (dispatched by the main loop via `JPP_VM_REQUEST_IDLE`)
- `handle_action(key)` — called when a key event fires; `key` is a `jpp_sdk_key_event_t` int constant (dispatched by the keypad task via `JPP_VM_REQUEST_ACTION`)

**Native C apps** — there are no separate lifecycle hooks. `jpp_app_entry(ctx)` is called once; the function owns the loop, polls keys with `jpp_sdk_poll_key` / `jpp_sdk_wait_key`, and returns when the app is done.

The app contract is intentionally narrow: app code should work through the App SDK and not assume privileged access to storage, network, time, or hardware beyond granted capabilities.

## Entry point contract

The entry point differs by app type.

**MicroPython entry (`main.py` compiled to `main.mpy`):**

MicroPython apps expose `create_app(sdk)` — a callable that accepts the SDK context and returns an app object. The runtime calls this once at launch, then dispatches lifecycle hooks (`on_start`, `on_foreground`, `on_background`, `on_idle`, `on_stop`, `handle_action`) on the returned object.

```python
import jppsdk

def create_app(sdk):
    return MyApp(sdk)

class MyApp:
    def __init__(self, sdk):
        self.sdk = sdk

    def on_start(self):
        self.sdk.set_frame(["Hello, world!"])

    def on_foreground(self):
        pass

    def on_background(self):
        pass

    def on_idle(self):
        key = self.sdk.poll_key()
        if key == jppsdk.KEY_CENTER:
            self.sdk.request_close()

    def on_stop(self):
        pass
```

**Native C entry:**

Native apps export a single function `jpp_app_entry`. The loader (`jpp_native_loader_core`) resolves it by name from the ELF symbol table and calls it directly. The function runs synchronously and returns when the app is done — there are no separate lifecycle hooks.

```c
#include "jpp_sdk_bridge.h"

void jpp_app_entry(jpp_sdk_context_t *ctx)
{
    /* draw initial frame */
    const char *lines[] = { "Hello, world!" };
    jpp_sdk_set_frame(ctx, lines, 1);

    /* poll keys until the user exits */
    jpp_sdk_key_event_t key;
    while (true) {
        jpp_sdk_wait_key(ctx, 100, &key);
        if (key == JPP_SDK_KEY_CENTER) {
            break;
        }
    }

    jpp_sdk_request_close(ctx);
}
```

## App SDK for MicroPython (`jppsdk` module)

MicroPython apps import `jppsdk` to access the App SDK. The same underlying C functions run whether the caller is a Python or a native app, so the two language surfaces stay in lock-step.

### Compilation

Compile Python source to `.mpy` bytecode with mpy-cross 1.28.0:

```bash
mpy-cross -march=rv32imc -O2 main.py   # produces main.mpy
```

The `-march=rv32imc` flag targets the RV32 RISC-V instruction set used by the ESP32-C6. Place the resulting `.mpy` file in the app's SD directory alongside `manifest.json`.

### Exceptions

| Exception | When raised |
|---|---|
| `jppsdk.SdkError` | Any non-OK SDK status |
| `jppsdk.SdkPermissionError` | `ACCESS_DENIED` — capability not granted |
| `ImportError` | Import blocked by the VM policy (e.g. `machine`, `os`) |

`SdkPermissionError` is a subclass of `SdkError`.

### Functions

**Core**

| Function | Signature | Notes |
|---|---|---|
| `set_frame` | `(lines: list[str])` | Render up to 7 text rows (≈21 chars each). Silently truncates long lines. |
| `request_close` | `()` | Signal the runtime to close this app. |
| `log` | `(event_name: str)` | Emit a tagged log event to the device log. |

**High-level UI** (no capability required)

These are blocking modal helpers — they take over the screen and the d-pad until the user resolves them, then return. The control scheme is uniform: the d-pad moves focus, CENTER selects/confirms/types, and a long-press CENTER is BACK (dismiss/cancel).

| Function | Signature | Returns |
|---|---|---|
| `dialog` | `(text: str, title: str = None)` | `True` if the user pressed OK, `False` on BACK |
| `list` | `(items: list[str], title: str = None, multiselect: bool = False)` | single-select: the chosen index `int`; multiselect: `list[int]` of checked indices; `None` if cancelled |
| `input` | `(title: str, placeholder: str = None, type: int = jppsdk.INPUT_TEXT)` | the typed `str`, or `None` if cancelled |

When a `title` is supplied, `dialog`, `list`, and the permission/confirm prompts
draw the shared **signature line** (a 1-px rule on page 1) under the title, so
modal prompts match the launcher, Settings, and WebDAV header style.

For text-style input (`INPUT_TEXT`, `INPUT_NUMBER`), `input` shows a graphical
on-screen keyboard: the `title` sits at the top, the current value appears in an
outlined field, and the keys are drawn below with the selected key highlighted
in-place (the highlight inverts the key, so moving the cursor never shifts the
layout). `INPUT_TEXT` is digits row + QWERTY letters + Shift + space/backspace/
done/cancel; `INPUT_NUMBER` is digits with `-` and `.`. Every keyboard layout
includes a **done** (`*`) key and a dedicated **cancel** (`X`) key.

`INPUT_DATE` and `INPUT_TIME` instead show a **field spinner**: LEFT/RIGHT move
between fields (year/month/day, or hour/minute/second — seconds are always
shown), UP/DOWN adjust the focused field by one (with month-length and leap-year
clamping), and a button row offers **123** (masked manual entry via the number
keyboard), **now** (fill from the RTC), **Cancel**, and **OK**. CENTER on a field
or on **OK** confirms. `INPUT_DATE` returns `"YYYY-MM-DD"`; `INPUT_TIME` returns
`"HH:MM:SS"`.

Cancelling any `input` — like a long-press CENTER (BACK) — returns `None`. For
`list`, a single-select choice returns on CENTER; multiselect uses CENTER to
toggle items and a trailing **Done** row to confirm.

```python
import jppsdk

if jppsdk.dialog("Erase all saved data?", title="Confirm"):
    name = jppsdk.input("Your name", placeholder="nickname")
    choice = jppsdk.list(["Red", "Green", "Blue"], title="Pick a colour")
    tags = jppsdk.list(["work", "home", "fun"], title="Tags", multiselect=True)
```

**Wakelock**

| Function | Signature | Notes |
|---|---|---|
| `wakelock_acquire` | `()` | Prevent the OS from dimming/sleeping while this app is active. |
| `wakelock_release` | `()` | Release the wakelock. Must be called to re-enable OS power management. |

**Buzzer** (no capability required)

| Function | Signature | Notes |
|---|---|---|
| `buzzer_play` | `(sound: int)` | Play a predefined sound: `SOUND_SUCCESS`, `SOUND_FAILURE`, `SOUND_NOTIFY`, `SOUND_STARTUP`, `SOUND_CLICK`. Non-blocking (returns immediately; sound stops in background). |
| `buzzer_tone` | `(freq_hz: int, duration_ms: int)` | Play a single tone. `freq_hz=0` is silence. Blocking for `duration_ms`. |
| `buzzer_play_sequence` | `(notes: list[tuple])` | Play a list of `(freq_hz, duration_ms)` tuples. Blocking. |
| `buzzer_stop` | `()` | Stop any playing sound immediately. |

**Device**

| Function | Signature | Returns | Capability |
|---|---|---|---|
| `device_status` | `()` | `dict` with `battery_pct` (`-1` if unknown) and `charging` | `device.status` |
| `get_time` | `()` | `str` `"YYYY-MM-DD HH:mm"` | `device.status` |

**Input**

| Function | Signature | Returns |
|---|---|---|
| `poll_key` | `()` | `int` key constant, or `KEY_NONE` if queue is empty |
| `wait_key` | `(timeout_ms: int)` | `int` key constant, or `KEY_NONE` on timeout |

Key constants: `KEY_NONE`, `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`, `KEY_CENTER`, `KEY_CENTER_LONG`.

**Scoped file I/O** (requires `files.scoped`)

| Function | Signature | Returns |
|---|---|---|
| `file_read` | `(path: str)` | `dict` with broker result fields |
| `file_write` | `(path: str, text: str)` | `None` |
| `file_list` | `(dir: str)` | `dict` with broker result fields |

Paths are relative to `/sd/apps/<app_id>/`.

**Shared file I/O** (requires `files.shared`)

| Function | Signature | Returns |
|---|---|---|
| `shared_read` | `(path: str)` | `dict` with broker result fields |
| `shared_write` | `(path: str, text: str)` | `None` |
| `shared_list` | `(dir: str)` | `dict` with broker result fields |

Paths are relative to `/sd/shared/<app_id>/`.

**Full file handles** (requires `files.full`)

| Function | Signature | Returns |
|---|---|---|
| `file_open` | `(path: str, mode: int)` | `int` handle |
| `handle_read` | `(handle: int)` | `dict` |
| `handle_write` | `(handle: int, text: str)` | `None` |
| `handle_list` | `(handle: int)` | `dict` |
| `handle_close` | `(handle: int)` | `None` |

Mode constants: `OPEN_READ = 0`, `OPEN_WRITE = 1`. Paths are absolute under `/sd/`. A user-approval prompt is shown before the handle is granted.

**BLE scan** (requires `ble.scan`)

| Function | Signature | Returns |
|---|---|---|
| `ble_scan` | `(duration_ms: int)` | `list[dict]` — each entry has `address`, `name`, `rssi`, `ad_data` (bytes) |

**BLE advertise** (requires `ble.advertise`)

| Function | Signature |
|---|---|
| `ble_advertise_start` | `(payload: bytes)` |
| `ble_advertise_stop` | `()` |

**BLE GATT client** (requires `ble.connect`)

| Function | Signature | Returns |
|---|---|---|
| `ble_connect` | `(address: str)` | `int` conn handle |
| `ble_read_char` | `(conn: int, svc_uuid: str, char_uuid: str)` | `dict` |
| `ble_write_char` | `(conn: int, svc_uuid: str, char_uuid: str, text: str)` | `None` |
| `ble_disconnect` | `(conn: int)` | `None` |

**BLE GATT server** (requires `ble.host`)

| Function | Signature |
|---|---|
| `ble_service_register` | `(svc_uuid: str)` |
| `ble_service_unregister` | `()` |

**HTTP** (requires `http.request`)

| Function | Signature | Returns |
|---|---|---|
| `http_request` | `(method: str, url: str, body: str \| None = None)` | `dict` with `status_code` and `body` |

`method` must be `"GET"` or `"POST"`. `body` may be `None` for GET requests.

**Text frame** (no capability required)

`set_frame(lines)` sets up to `JPP_RESOURCE_SDK_FRAME_LINE_LIMIT` (7)
content rows (rows 0–6), all of which are app-controlled — there is no
firmware-injected status bar. Extra lines beyond the limit are dropped and
the call reports `TEXT_TRUNCATED`.

**Canvas pixel drawing** (no capability required)

The canvas is a 128×48 pixel content area (display pages 2–7). Canvas pixels are rendered on top of text frame lines.

| Function | Signature | Notes |
|---|---|---|
| `canvas_write` | `(row: int, pixels: bytes)` | Write 16-byte row (0–47); each byte is 8 pixels, MSB = leftmost |
| `canvas_clear` | `()` | Zero the entire canvas |
| `canvas_draw_pixel` | `(x: int, y: int, on: bool)` | Set or clear a single pixel; x: 0–127, y: 0–47 |

**IPC** (requires `ipc.send`)

| Function | Signature | Returns |
|---|---|---|
| `ipc_send` | `(recipient_id: str, payload: str)` | `None` |
| `ipc_recv` | `()` | `(payload: str, sender: str)` tuple, or `None` if no messages |

Messages are file-backed; `ipc_recv` consumes and deletes the oldest message.

**Background tasks** (requires `background.register`, system-granted)

| Function | Signature |
|---|---|
| `add_background_task` | `(name: str)` |

**Persistent key-value store** (requires `device.kv`)

Per-app store backed by `/data/kv/<app_id>.json`; survives app restarts.

| Function | Signature | Returns |
|---|---|---|
| `kv_get` | `(key: str)` | `str` value, or `None` if key does not exist |
| `kv_set` | `(key: str, value: str)` | `None` |
| `kv_delete` | `(key: str)` | `None` |

### Manifest for a MicroPython app

```json
{
  "schema_version": 2,
  "app_id": "demo_clock",
  "name": "Demo Clock",
  "version": "1.0.0",
  "sdk_min": 1,
  "sdk_max": 1,
  "entry": "main.mpy",
  "app_type": "micropython",
  "capabilities": ["device.status", "device.kv"],
  "background": { "enabled": false, "mode": "serialized" },
  "toolchain": {
    "runtime_version": "v1.28.0",
    "cross_version": "1.28.0",
    "bytecode_abi": 6
  }
}
```

Allowed capabilities for `device.kv` and `http.request` are Tier 1 — they require a one-time user grant. Declare them in `capabilities`; the user is asked to approve them when the app launches, and the grant is then remembered for future launches.

## Rendering and UI contract

Apps render through the runtime-managed frame model rather than by touching display hardware directly. The App SDK exposes `set_frame` for text lines and `canvas_write` for raw pixel rows in the 128×48 content area — both available without any capability declaration.

For interactive prompts, use the high-level App SDK helpers — `dialog`, `list`, and `input` — instead of building your own from `set_frame` + `wait_key`. They are blocking modals that own the screen and the d-pad until the user resolves them, and they give every app a consistent look and control scheme. The firmware reuses the same `dialog` helper to render the `files.full` per-path approval prompt.

Keep UI behaviour aligned with these helpers and the launcher-managed frame rules; never assume direct display or broker access.

## Native development facts

- The repo is an ESP-IDF firmware tree, not a package-manager app bundle.
- `components/jpp_core/` contains the native implementation surface.
- The manifest contract is schema v2 for production app packages.
- The capability model and broker concepts remain part of the runtime security boundary.
- Error codes are part of the native contract and should remain stable across docs and runtime logs.

## Notes on app packaging

App packaging should follow the manifest rules above. Keep app layout and package validation aligned with the production architecture and the core component behavior.

## What to avoid

- Do not describe `firmware/` as the active app source tree.
- Do not bypass the broker for file, network, keypad, RTC, or storage access in any app type.
- Do not compile `.mpy` files with a different mpy-cross version than 1.28.0 — the bytecode ABI check will reject them at load time.
- Do not use `network.bind` until it is implemented — it is whitelisted in the manifest validator but has no runtime implementation yet.
- Do not mix `canvas_write`/`canvas_draw_pixel` with `set_frame` on the same lines — canvas pixels overlay text; use one or the other for the content area.

## Summary

Use the native ESP-IDF toolchain, the Docker-based `idf.py` workflow, and the component architecture in `components/jpp_core/` as the development baseline. Keep app contracts centered on manifest schema v2, capabilities, broker policy, and error codes.
