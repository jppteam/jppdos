# Manifest reference

Every app package contains a `manifest.json` that tells the firmware who the app is, how to run it, what permissions it needs, and whether it can run background tasks. The firmware validates the manifest before the app appears in the launcher; invalid manifests are silently rejected.

---

## Schema v2 — complete field reference

```json
{
  "schema_version": 2,
  "app_id": "my_app",
  "name": "My App",
  "version": "1.0.0",
  "author": "Jane Dev",
  "sdk_min": 1,
  "app_type": "micropython",
  "entry": "main.mpy",
  "capabilities": [],
  "background": {
    "enabled": false,
    "tasks": []
  },
  "toolchain": {
    "runtime_version": "v1.28.0",
    "cross_version": "1.28.0",
    "bytecode_abi": 6
  }
}
```

### `schema_version`

**Required.** Must be the integer `2`. Any other value rejects the manifest.

### `app_id`

**Required.** A stable, unique identifier for the app. Rules:
- Dot-separated identifier segments (`my_app`, `com.example.clock`)
- All lowercase; only letters, digits, underscores, and dots
- Cannot be a reserved built-in name: `launcher`, `settings`, `webdav`, `dialogs`, `files`, `about`
- The `app_id` determines the app's storage paths (`/sd/apps/<app_id>/`, `/sd/shared/<app_id>/`) — changing it in a new version loses all stored data

### `name`

**Required.** The display name shown in the launcher. Keep it short enough to fit on screen (~16 characters or fewer).

### `version`

**Required.** A version string shown in the launcher and device info screens. Any format is accepted (e.g. `"1.0.0"`, `"2024-06"`, `"beta"`).

### `author`

**Optional.** A free-form string naming the app's author or publisher (e.g.
`"jppdos"`, `"Jane Dev"`). Purely informational: the current firmware does
**not** read, validate, or verify this field in any way — it is metadata for
humans and tooling only. Any string value is accepted and unknown-to-firmware
fields are ignored, so declaring it never affects loading or capabilities.

### `sdk_min`

**Required.** The minimum SDK API level the app needs, as an integer `≥ 1`. The
firmware exports one SDK level (`JPP_SDK_VERSION`, currently **3**); the loader
rejects an app whose `sdk_min` is greater than the running level with
`SDK_TOO_OLD`. There is no upper bound: the SDK surface only ever grows in a
backward-compatible way, so an app built for an older level keeps running on
newer firmware. Declare the lowest level that provides every SDK symbol and
capability your app uses:

| `sdk_min` | Declare it if you use | Since firmware |
|-----------|-----------------------|----------------|
| `1` | the original SDK surface | v1.0-RTM |
| `2` | outbound TCP (`net_connect`, capability `network.connect`) · TLS-verified HTTP (`https_request`, capability `https.request`) · the crypto primitives (`jpp_crypto_sha256`/`sha1`, `jpp_crypto_aes256_ige_*`, `jpp_crypto_modexp`/`rsa_encrypt`/`dh_compute`) · CENTER gesture claims (`claim_center`, `KEY_BACK`, `KEY_CENTER_HOLD`/`_DOUBLE`) · `jpp_sdk_confirm` from a native app | v1.1 |
| `3` | `jpp_sdk_wrap_text` from a native app | *unreleased* |

Declaring `sdk_min: 2` means the app will not load on a v1.0-RTM device. If you
only use the original surface, leave it at `1` so the app runs on every unit.

Level 3 is **not yet in any released firmware**, so an app declaring `sdk_min: 3`
is rejected with `SDK_TOO_OLD` on every unit currently in the field.

`sdk_min` gates **symbols and capabilities only**. It says nothing about how much
memory a device will give your app: the [app pool](sdk/limits.md#apps-and-background)
grew from 64 KB to 80 KB in the same firmware that exports level 3, but there is
no manifest field for that and declaring `sdk_min: 3` does not reserve it. An app
too large for the running firmware's pool fails at load with `NO_MEMORY`, not
`SDK_TOO_OLD`.

The [SDK changelog](sdk-changelog.md) documents each level in full.

### `app_type`

**Required.** Selects the runtime:

| Value | Entry file format | Runtime |
|-------|-------------------|---------|
| `"micropython"` | `.mpy` compiled bytecode | MicroPython 1.28.0 interpreter |
| `"native"` | `.bin` ELF32 RISC-V shared object | Native loader (`jpp_native_loader_core`) |

### `entry`

**Required.** Relative path to the entry file within the app directory.
- MicroPython: usually `"main.mpy"`
- Native: usually `"<app_id>.bin"`

### `capabilities` { #field-capabilities }

**Optional.** Array of capability strings the app may use. Only declared capabilities can be granted. An app may declare up to 16 capabilities.

See [Capabilities](#capabilities) below for the full list.

### `background`

**Optional.** Controls headless background task scheduling.

```json
"background": {
  "enabled": true,
  "tasks": [
    { "name": "sync", "interval_s": 300 },
    { "name": "digest", "cron": "0 8 * * *" }
  ]
}
```

**`enabled`** — `true` opts the app in to background scheduling. The `background.register` capability must also be declared, and the user must grant it.

**`tasks`** — Array of scheduled tasks. Maximum 8 across all apps on the device. Each task must have:
- `name` — identifier passed to the task handler as a string
- Either `interval_s` (integer seconds, minimum 60) **or** `cron` (5-field cron expression, see below)

**Cron format:** 5 space-separated fields: `minute hour day-of-month month day-of-week`. Each field is either `*` (every) or a single integer. The scheduler fires the task once in any matching minute while the device is awake; deep sleep pauses the scheduler and missed cron minutes are not replayed.

Examples:
- `"0 * * * *"` — top of every hour
- `"30 6 * * *"` — 06:30 every day
- `"0 9 * * 1"` — 09:00 every Monday

### `toolchain`

**Required for MicroPython apps; omitted for native apps.**

Must be exactly:

```json
"toolchain": {
  "runtime_version": "v1.28.0",
  "cross_version": "1.28.0",
  "bytecode_abi": 6
}
```

The firmware checks all three values. A `.mpy` compiled with a different version of mpy-cross will fail the bytecode ABI check and the app will not load.

---

## Capabilities

Capabilities are declared in the `capabilities` array. The user is prompted for consent the first time the app calls an SDK function that needs that capability — not at launch.

There are **two tiers** of consent:

!!! info "Tier 1 — one-time grant, persisted."
    The user sees the prompt once. If they allow it, the grant is saved to
    `/data/grants/<app_id>.json` and the app is not asked again on future
    launches. If denied, the SDK call returns an error and the app keeps
    running.

!!! warning "Tier 2 — per-session grant, never persisted."
    The user sees the prompt once per launch. If denied, the SDK call errors; if
    allowed, it stays allowed until the app exits — and is asked again next
    time.

!!! danger "A denied capability is a normal outcome."
    The user can say no to anything, at any time, and your app keeps running.
    Handle `ACCESS_DENIED` / `SdkPermissionError` by degrading — showing a
    message, disabling a feature — never by crashing.

### Capability table

| Capability | Tier | `sdk_min` | What it unlocks |
|------------|------|-----------|-----------------|
| `http.request` | 1 | 1 | Broker-serialized HTTP GET and POST requests via [`http_request`](sdk/network.md#http_request) (cleartext only) |
| `https.request` | 1 | **2** | TLS-verified HTTPS requests via [`https_request`](sdk/network.md#https_request). **Additionally prompts once per origin** — see below |
| `ble.scan` | 1 | 1 | Passive BLE scan — discover nearby devices, read advertisement payloads and RSSI |
| `ble.advertise` | 1 | 1 | Broadcast a raw BLE advertisement payload; also enables `ble_set_connectable` |
| `background.register` | 1 | 1 | Enroll the manifest's `background.tasks` schedule for headless background execution |
| `esp_now` | 1 | 1 | Send and receive ESP-NOW packets ([`espnow_send`](sdk/wireless.md#espnow_send)/[`espnow_recv`](sdk/wireless.md#espnow_recv)) |
| `files.full` | 2 | 1 | Full SD card access via [`file_open`](sdk/storage.md#file_open) — each path the app opens gets its own per-path approval prompt |
| `network.bind` | 2 | 1 | Open a TCP server socket: one listener, up to 2 accepted connections |
| `network.connect` | 2 | **2** | Open an outbound TCP connection via [`net_connect`](sdk/network.md#net_connect); shares the connection table with `network.bind` |
| `ble.connect` | 2 | 1 | GATT client: connect to a BLE peripheral, read and write characteristics |
| `ble.host` | 2 | 1 | GATT server: register one service and accept inbound BLE connections |

Declaring a capability that is not in this table causes the manifest to be rejected with `INVALID_CAPABILITY`.

### Resource-scoped consent

Two capabilities are not a blanket grant — holding them still leaves the user in control of *what* the app reaches:

- **`files.full`** prompts again for every path the app opens.
- **`https.request`** prompts again for every new **origin** (`scheme://host[:port]`). Unlike the per-path prompt, an approved origin is persisted to `/data/grants/<app_id>.origins`, so the user is asked once per host and never again. An app that later starts contacting a different host has to ask afresh.

Both are deliberate: the capability answers "may this app use the network / the filesystem at all", and the second prompt answers "with whom", which is the question the user can actually judge.

### Ungated surface (no capability needed)

These work in every app, with no manifest declaration and no user prompt:

- Drawing: `set_frame`, `canvas_write`, `canvas_draw_pixel`, `canvas_clear`, `canvas_fullscreen`
- Input: `poll_key`, `wait_key`
- High-level UI helpers: `dialog`, `list`, `input`, `confirm`, `file_pick` — though `file_pick` internally requires `files.full`
- Buzzer: `buzzer_play`, `buzzer_tone`, `buzzer_play_sequence`, `buzzer_play_sequence_async`, `buzzer_stop`
- LED: `led_set_color`, `led_off` (onboard WS2812 pixel)
- Wakelock: `wakelock_acquire`, `wakelock_release`
- Device info: `device_status`, `get_time`, `is_dummy_mode`
- CENTER gesture claims: `claim_center` (SDK level 2)
- Crypto primitives: `jpp_crypto_sha256`/`sha1`, `aes256_ige_*`, `modexp`/`rsa_encrypt`/`dh_compute` (SDK level 2, C only)
- Scoped file I/O: `file_read`, `file_write`, `file_list` (sandboxed to `/sd/apps/<app_id>/`)
- Shared file I/O: `shared_read`, `shared_write`, `shared_list` (sandboxed to `/sd/shared/<app_id>/`)
- Key-value store: `kv_get`, `kv_set`, `kv_delete`
- IPC: `ipc_send`, `ipc_recv`
- Lifecycle: `request_close`, `log`
- Code modules (native only): `module_load`, `module_run`, `module_unload`

---

## Complete examples

### Minimal MicroPython app

```json
{
  "schema_version": 2,
  "app_id": "hello",
  "name": "Hello World",
  "version": "1.0.0",
  "author": "Jane Dev",
  "sdk_min": 1,
  "app_type": "micropython",
  "entry": "main.mpy",
  "capabilities": [],
  "background": { "enabled": false },
  "toolchain": {
    "runtime_version": "v1.28.0",
    "cross_version": "1.28.0",
    "bytecode_abi": 6
  }
}
```

### MicroPython app with HTTP and background sync

```json
{
  "schema_version": 2,
  "app_id": "weather",
  "name": "Weather",
  "version": "2.1.0",
  "sdk_min": 1,
  "app_type": "micropython",
  "entry": "main.mpy",
  "capabilities": ["http.request", "background.register"],
  "background": {
    "enabled": true,
    "tasks": [
      { "name": "fetch", "interval_s": 1800 }
    ]
  },
  "toolchain": {
    "runtime_version": "v1.28.0",
    "cross_version": "1.28.0",
    "bytecode_abi": 6
  }
}
```

### Minimal native C app

```json
{
  "schema_version": 2,
  "app_id": "counter",
  "name": "Counter",
  "version": "1.0.0",
  "author": "Jane Dev",
  "sdk_min": 1,
  "app_type": "native",
  "entry": "counter.bin",
  "capabilities": [],
  "background": { "enabled": false }
}
```

Note: native apps omit the `toolchain` block.
