# ESP-IDF Native/App Contract

*Firmware v1.0-RTM · 2026-07-16*

This document defines the native/app boundary for JPPDOS. The native ESP-IDF core in `components/jpp_core/` owns boot sequencing, storage, settings, broker policy, hardware drivers, and the UI/runtime bridge. Apps interact with the system exclusively through the App SDK and broker services.

## Boot order

1. Storage initializes, creating the base writable layout for `/data` and `/lib` before any settings or UI work.
2. Settings load from `/data/settings.json` under schema v2 rules. If only `/data/settings.json.tmp` exists, it is promoted and `SETTINGS_RECOVERED` is emitted.
3. `/sd` mount is attempted before app discovery.
4. Runtime path wiring for `/lib` and `/sd/lib` happens before any MicroPython app imports.
5. Boot mode is `normal` only when `/sd` is mounted and recovery is not forced. Otherwise boot mode is `recovery`.
6. The native broker initializes OLED, RTC, SD, network, and keypad services before discovery.
7. App discovery runs after broker init. Recovery mode exposes only the built-in entries; normal mode adds discovered SD apps and counts rejected packages.
8. `SYSTEM_READY` is emitted before launcher handoff.

## Settings storage model

Persistent state is split between one JSON file and NVS namespaces:

- `/data/settings.json` (schema v2, with `/data/settings.json.tmp` as the staging/recovery file) holds exactly: `schema_version`, `policy.wifi.preferred_ssid`, `policy.wifi.password`, and `policy.recovery.force_recovery`.
- Everything else lives in NVS and NVS is authoritative for it: `jpp_time` (NTP enable/host/timezone), `jpp_power` (dim/poweroff seconds), `jpp_webdav` (password mode + static password), `jpp_sound` (buzzer volume, startup jingle), `jpp_user` (username). Limited Run Verification data is the exception: it lives on the external AT24C32 EEPROM, not in NVS or settings.json, so it survives factory reset and reflash.

Rules:

- `schema_version` is `2`. Any other schema — or a corrupt/non-dict payload — is treated as corrupt: `SETTINGS_RESET` is emitted, defaults are rewritten, and boot continues. There is no migration path.
- Writes to settings.json are atomic: staged to `/data/settings.json.tmp`, then renamed over the main file. Temp-file recovery promotes the staging file when it is the only settings file present, so a failed write never loses the last complete payload.
- `Settings > Backup settings` serializes the NVS namespaces plus settings.json into one backup file under `/sd/backups/`; restore writes them back and restarts.

## Logical paths

| Logical path | Owner | Contract |
| --- | --- | --- |
| `/data` | Native storage layer | Flash-backed writable state for settings, grants (`/data/grants/<app_id>.json`), and the crash log (`/data/ui_crash.log`). Maps to the ESP-IDF `data_fs` mount. |
| `/lib` | Native runtime layer | System-owned shared library/runtime path. Maps to the ESP-IDF `runtime_fs` mount. |
| `/sd` | Removable media layer | Removable app/media root; absence forces recovery-compatible behavior. A distinct removable mount, never a flash alias. |
| `/sd/apps/<dir>` | App packaging | Manifest-driven app package root. Packages are discovered only under this subtree. |
| `/sd/lib` | Runtime layer | Optional shared SD library root; system-owned, not a promise of unrestricted imports. |
| `/sd/apps/<app_id>/` | App SDK (scoped storage, ungated) | App-private writable subtree on SD. `file_read()` / `file_write()` / `file_list()` paths are rooted here; the KV helper persists to `.kv.json` inside it. |
| `/sd/shared/<app_id>/` | App SDK (shared storage, ungated) | Per-app subdirectory within the shared SD area; any app can read another app's shared subtree through `shared_*()`. |
| `/sd/ipc/<recipient>/<sender>/` | App SDK (IPC, ungated) | Mailbox files (`msg_<tick>.json`) written by `ipc_send()` and consumed (then deleted) by the recipient's `ipc_recv()`. |

### ESP-IDF mount contract

- `data_fs` is the flash-backed ESP-IDF VFS mount that backs `/data`.
- `runtime_fs` is the flash-backed ESP-IDF VFS mount that backs `/lib`.
- `/sd` is mounted independently from removable media and is never folded into either flash mount.

### Path validation rules

- Manifest and SDK-relative package paths must be relative, must not start with `/`, and must not contain `..` traversal segments.
- Package entry paths are resolved relative to `/sd/apps/<dir>` and must stay within the app root.
- Scoped paths are rooted at `/sd/apps/<app_id>/` and shared paths at `/sd/shared/<app_id>/`; the SDK builds the absolute path itself, so path construction alone enforces the sandbox root.
- `files.full` SDK paths must be absolute under `/sd/` with no `..` traversal; each path requires per-open user approval.
- Any path outside these rooted logical spaces is rejected rather than normalized into a broader filesystem escape.

## Capability model

Two kinds of SDK surface exist:

- **Ungated** — available to every app with no manifest declaration and no consent tracking: frame/canvas rendering, key input, dialogs (`dialog`/`list`/`input`), buzzer, wakelock, scoped storage, shared storage, the KV helper, IPC, `device_status()`, and `get_time()`. The firmware enforces *scoping* (sandbox roots, budgets), not permission.
- **Declared** — must be listed in the manifest `capabilities` array and consented by the user on first use (lazy / per-use prompts via `jpp_sdk_ensure_cap`):
  - **Tier 1** (prompted once, persisted to `/data/grants/<app_id>.json`): `http.request`, `ble.scan`, `ble.advertise`, `background.register`, `esp_now`.
  - **Tier 2** (prompted on first use every launch, never persisted): `files.full`, `network.bind`, `ble.connect`, `ble.host`.

A denied capability stays out of the broker caller list; the broker is the final authority. Every denial is logged (`CAP_DENIED` for user-declined or undeclared capabilities, `ACCESS_DENIED` from the broker gate).

## Broker ownership

| Resource | Native owner | App-visible path |
| --- | --- | --- |
| OLED + shared I2C bus | Broker serializes access and owns rendering | Apps update text through SDK frame APIs, not driver handles. |
| RTC/time, battery | Broker owns time reads and ADC reads | `device_status()` returns battery state; `get_time()` returns RTC time. Both ungated. |
| Files on `/sd` (scoped/shared) | Broker validates logical paths and mediates reads/writes under the `storage` lock | `file_*()` rooted at `/sd/apps/<app_id>/`; `shared_*()` rooted at `/sd/shared/<app_id>/`. Ungated. |
| Files on `/sd` (full) | Broker validates absolute paths; each open requires user approval | `files.full` gates `file_open()` / `handle_*()` / `file_pick()`. Handles are valid for the app session. |
| HTTP client | Broker serializes requests under the `http` lock | `http.request` gates `http_request()`. |
| TCP server sockets | Broker serializes socket calls under the `network` lock; lwIP callbacks live in `main/jpp_native_services.c` | `network.bind` gates `net_bind()`/`net_accept()`/`net_recv()`/`net_send()`/`net_close()`; 1 listener + 2 connections; binding refused while WebDAV or the LRV server runs; all sockets closed at app teardown. |
| BLE radio | Broker serializes radio, client, and host access under the `ble_radio` / `ble_client` / `ble_host` locks | `ble.scan` / `ble.advertise` / `ble.connect` / `ble.host` gate the corresponding `ble_*()` calls. |
| Keypad + launcher focus | Launcher/system UI owns input polling | Apps receive normalized actions and lifecycle callbacks, not raw keypad driver access. |
| Recovery/settings policy | Native core + Settings UI only | Runtime apps do not mutate recovery policy directly. |

Named broker locks: `storage`, `device`, `vm_queue`, `ble_radio`, `ble_client`, `ble_host`, `http`, `network`.

## SSD1306 text UI

- The launcher is a text-first SSD1306 shell with a root screen, built-in Settings and WebDAV entries, line-based rendering, and crash recovery back to the launcher.
- Rendering is line-based and satisfies an eight-line frame contract.
- Settings is a built-in system UI entry available in both normal and recovery mode.
- App failures (MicroPython runner errors, native load failures) emit `APP_CRASH`, append a line to `/data/ui_crash.log`, and surface a crash dialog after teardown; the shell keeps running.

## Single shared VM

- Exactly one embedded VM exists for MicroPython apps.
- The native core never calls app objects from arbitrary RTOS tasks; lifecycle hooks are marshaled onto the shared VM task.
- Exactly one app may own the foreground at a time.
- Background work is opt-in and never concurrent with a foreground app: scheduled tasks (manifest `background.tasks` + persisted `background.register` grant) run as separate headless sessions while the device idles on the launcher.
- Built-in system UI flows such as Settings are native/system-owned and do not run inside the shared VM.

## Resource budget slice

The budget slice is defined in `components/jpp_core/include/jpp_resource_budget.h` and enforced at the points named below; host probes under `tests/` assert the same numbers.

| Area | Target | Limit / floor | Failure behavior |
| --- | --- | --- | --- |
| VM heap | `131072` bytes | reject configs above `196608` bytes | `jpp_vm_runtime_prepare()` returns `INVALID_CONFIG`. |
| VM stack | `8192` words | reject configs above `12288` words | `jpp_vm_runtime_prepare()` returns `INVALID_CONFIG`. |
| VM queue | `4` requests | reject configs above `8` requests | Oversubscription returns `QUEUE_FULL`; over-limit config returns `INVALID_CONFIG`. |
| Native free heap after boot | floor of `65536` bytes | `jpp_heap_monitor` warns at 30 KB and errors at 15 KB free | Failed allocations are logged with size/caps/function (`ALLOC FAILED`, tag `heap_mon`). |
| SDK frame bridge | `7` lines | `7` lines | Extra frame lines return `TEXT_TRUNCATED`. |
| Background schedule | `4` tasks per manifest | `8` schedule entries across all apps | Extra manifest tasks reject the manifest (`INVALID_BACKGROUND`); a full schedule table drops the overflowing entry with a `BG_SCHEDULE full` log. Each headless run is killed after `30000` ms (`BG_TASK_KILLED`, device restart); intervals below `60` s reject the manifest. |
| SDK file handles (`files.full`) | `2` open handles | `4` open handles | Further `file_open()` calls return `HANDLE_LIMIT`. |
| SDK BLE connections (`ble.connect`) | `1` open connection | `2` open connections | Further `ble_connect()` calls return `BLE_CONN_LIMIT`. |
| Broker result fields | `4` fields | `8` fields | Further fields return `FIELD_OVERFLOW`. |
| Broker locks | `4` named locks | `8` named locks | Further resources return `FIELD_OVERFLOW`. Named locks in use: `storage`, `device`, `vm_queue`, `ble_radio`, `ble_client`, `ble_host`, `http`, `network`. |

## Best-effort sandbox

- Runtime apps have a restricted import surface. The native bridge exposes explicit SDK modules without opening raw hardware modules.
- Apps do not get direct hardware, unrestricted OS, raw network, or arbitrary filesystem access.
- The broker is the final authority on file, time, and network access; declared capabilities are checked inside the shared VM and the bridge.
- App-private file access is rooted at `/sd/apps/<app_id>/` and shared access at `/sd/shared/<app_id>/` — both enforced by path construction. Arbitrary SD access (`files.full`) requires per-path user approval at open time.
- This is a best-effort sandbox for product stability and policy enforcement, not a malicious-code containment boundary.

## Manifest v2 package model

App packages are discovered from `/sd/apps/<dir>/manifest.json`. Each package must include a compiled entry module and a valid manifest.

| Field | Type | Rule |
| --- | --- | --- |
| `schema_version` | int | Must be `2`. |
| `app_id` | string | Stable unique app identifier; reserved ids (`launcher`, `settings`, `webdav`, `webdav_passconfig`, `shell`, `dialog`, `app_crash`, `sd_ejected`) are rejected. |
| `name` | string | Launcher label. |
| `version` | string | App package version string. |
| `sdk_min` | int | Inclusive minimum supported SDK version. |
| `sdk_max` | int | Inclusive maximum supported SDK version. |
| `entry` | string | Relative compiled entry path inside the app root; no leading `/` and no `..` traversal. `.mpy` for MicroPython apps, `.bin` for native apps. |
| `capabilities` | list[string] | Only the nine prompted capabilities are declarable — Tier 1: `http.request`, `ble.scan`, `ble.advertise`, `background.register`, `esp_now`; Tier 2: `files.full`, `network.bind`, `ble.connect`, `ble.host`. Anything else is `INVALID_CAPABILITY`. |
| `background.enabled` | bool | Enables or disables background execution for the app. |
| `background.tasks` | list | Up to `4` tasks, each `{name, interval_s}` or `{name, cron}` — exactly one schedule source per task. `interval_s` ≥ `60`; `cron` is 5-field `min hour dom mon dow`, each field `*` or a single in-range number. Violations reject the manifest (`INVALID_BACKGROUND`). |
| `toolchain.runtime_version` | string | MicroPython apps only: exact runtime version pin. |
| `toolchain.cross_version` | string | MicroPython apps only: exact `mpy-cross` version pin. |
| `toolchain.bytecode_abi` | int | MicroPython apps only: bytecode ABI version; mismatch rejects the app before launch. |

- `entry` is a relative package path; leading `/` and `..` traversal are rejected.
- `manifest.json` is the discovery source of truth for the app directory.
- `entry_path` is rooted under `/sd/apps/<dir>` and derived from the manifest, not provided as an absolute path.
- App entrypoints are compiled modules (`.mpy` bytecode or `.bin` ELF objects). Uncompiled source files are not loaded.
- MicroPython packages that omit toolchain metadata are rejected; native packages carry no toolchain block.

## SDK surface and versioning

- The native bridge exports SDK version `1`. The `sdk_min` / `sdk_max` range check is inclusive.
- Available from both C and MicroPython (`jppsdk` module): `set_frame()`, `request_close()`, `log()`, `dialog()`, `list()`, `input()`, `poll_key()`, `wait_key()`, `canvas_write()`, `canvas_clear()`, `canvas_draw_pixel()`, `canvas_fullscreen()`, `buzzer_tone()`, `buzzer_play()`, `buzzer_play_sequence()`, `buzzer_play_sequence_async()`, `buzzer_stop()`, `led_set_color()`, `led_off()`, `wakelock_acquire()`, `wakelock_release()`, `device_status()`, `get_time()`, `is_dummy_mode()`, `file_read()`, `file_write()`, `file_list()`, `shared_read()`, `shared_write()`, `shared_list()`, `file_open()`, `handle_read()`, `handle_write()`, `handle_list()`, `handle_close()`, `background_register()`, `http_request()`, `net_bind()`, `net_accept()`, `net_recv()`, `net_send()`, `net_close()`, `ipc_send()`, `ipc_recv()`, `kv_get()`, `kv_set()`, `kv_delete()`, `ble_scan()`, `ble_advertise_start()`, `ble_advertise_stop()`, `ble_connect()`, `ble_read_char()`, `ble_write_char()`, `ble_disconnect()`, `ble_service_register()`, `ble_service_unregister()`, `espnow_send()`, `espnow_recv()`.
- C-only: `jpp_sdk_confirm()` (the shared Deny/Allow consent surface), `jpp_sdk_file_pick()` (SD file browser, `files.full`), `jpp_sdk_wrap_text()`, `jpp_sdk_request_cap()` (proactively fires the consent prompt for one declared capability without doing any work), the code-module surface `jpp_sdk_module_load()` / `jpp_sdk_module_run()` / `jpp_sdk_module_unload()` (native apps only), and the GATT-host value operations `jpp_sdk_ble_host_set_value()` / `ble_host_wait_write()` / `ble_host_clear()` / `ble_set_connectable()`.
- The `dialog()`, `list()`, and `input()` helpers are the standard high-level UI prompts (message box, selection list, on-screen text/value entry) and are ungated. Modal helpers with a title draw the shared "signature line" rule on page 1 (matching the launcher/settings/WebDAV header). `input()` with `INPUT_DATE` / `INPUT_TIME` renders a field spinner (LEFT/RIGHT pick field, UP/DOWN adjust) with `123` (masked manual entry), `now` (RTC), Cancel, and OK buttons; it returns `YYYY-MM-DD` for dates and `HH:MM:SS` (seconds included) for times.
- `device_status()` returns `battery_pct` and `charging` (`-1` = unknown; the board has no charge-detect line); `get_time()` returns `"YYYY-MM-DD HH:mm"`. Both ungated. `kv_get()` returns a non-OK status when the key is absent (so a get after a delete reports "not found" rather than an empty value). `ipc_recv()` reports an empty mailbox as `NO_DATA` (MicroPython: returns `None`). `log()` writes the event to the serial console under tag `app_log`.
- `file_read()` / `file_write()` / `file_list()` use app-local relative-path semantics rooted at `/sd/apps/<app_id>/`.
- `shared_read()` / `shared_write()` / `shared_list()` use app-local relative-path semantics rooted at `/sd/shared/<app_id>/`.
- `file_open()` takes an absolute `/sd/` path, prompts the user for approval, and returns a session-scoped handle.
- Adding a new privileged surface or changing an existing method's behavior in a breaking way requires an SDK version bump and manifest range validation.

## App lifecycle

- App discovery lists every `/sd/apps/<dir>` with a `manifest.json` and a non-reserved id; rejected directories are counted and logged.
- MicroPython apps export `create_app(sdk)` — the MicroPython runner calls it once and dispatches lifecycle hooks (`on_start`, `on_stop`, `on_idle`, `handle_action`) on the returned object.
- Native apps export `void jpp_app_entry(jpp_sdk_context_t *ctx)` — the ELF loader (`jpp_native_loader_core`) resolves and calls it directly; the function runs synchronously with no separate lifecycle hooks.
- `BACK` requests close; the session is torn down when the app's entry returns or `request_close()` is called.
- Opening an app that already has a live session foregrounds the existing session instead of starting a second copy.

## Background scheduler

- Schedules are declarative: the manifest's `background.tasks` is the only schedule source. `jpp_sdk_background_register()` / `jppsdk.background_register()` triggers the `background.register` consent prompt (Tier 1); while that grant is persisted, the firmware syncs the app's tasks into `/data/bg_schedule.json` at every app exit and removes them when the grant or manifest entry disappears.
- Due tasks run only while the device is idle on the launcher — no foreground app, no WebDAV or LRV server, no serial session. A user launch preempts a running task (`BG_TASK_PREEMPTED`).
- Headless runs reuse the normal app session machinery without UI: MicroPython runs the module-level `on_task(name)`; native apps run the optional `jpp_app_task_entry(ctx, name)` export (a missing export logs `NO_TASK_ENTRY`). Consent prompts are denied during headless runs (`CONSENT_HEADLESS_DENY`), so only launch-time persisted Tier-1 grants are usable.
- Interval tasks fire when `now >= last_run + interval_s`; cron tasks fire once within the matching minute. `last_run` is recorded **before** the run starts, so a crashing task cannot re-fire in a loop. The scheduler reads the DS1307 RTC and only ticks while the device is awake; deep sleep pauses it and missed cron minutes are not replayed.
- A run that exceeds `JPP_RESOURCE_BG_TASK_RUN_QUOTA_MS` (`30000` ms) is terminated by a device restart (`BG_TASK_KILLED`) — a mid-run task kill could leak broker locks or the app pool, so the firmware restarts instead of deleting the task.

## Failure taxonomy

| Failure class | Marker / code | Required response |
| --- | --- | --- |
| Bad package at discovery | `APP_REJECTED <dir>: RESERVED_APP_ID` or `manifest.json missing` | Reject only the bad directory, count it in `rejected`, keep booting, and still reach the launcher. |
| Bad manifest / entry at launch | `SD_APP_LAUNCH <id>: preflight <result>` where result is `MANIFEST_REJECTED` (with the manifest validator code, e.g. `SCHEMA_MISMATCH`, `INVALID_ENTRY`, `INVALID_CAPABILITY`, `RUNTIME_MISMATCH`), `MISSING_ENTRY`, `INVALID_ENTRY`, `INVALID_TOOLCHAIN`, or `CORRUPT` | Refuse the launch; the launcher stays alive and the catalog stays available. |
| Unsupported import | `UNSUPPORTED_IMPORT` (VM status) | The import is refused inside the shared VM; the app fails, the launcher does not. |
| App failure at runtime | `APP_CRASH app=<id> reason=<reason>` | Append a line to `/data/ui_crash.log`, tear down the session, and show the crash dialog before normal launcher flow resumes. |
| Background task failure | `BG_TASK_ERROR` (runner/loader error) or `NO_TASK_ENTRY` (native export missing) | Log only — no crash dialog for headless runs; the session is torn down and the launcher is unaffected. |
| Background task overrun | `BG_TASK_KILLED (quota exceeded)` | Restart the device; `last_run` was recorded at launch, so the task does not re-fire immediately after boot. |
| Capability denial | `CAP_DENIED` (user declined / not declared) or `ACCESS_DENIED` (broker gate) | Return an error result to the app, log the denial, and do not grant the privileged action. |
| SD removal during runtime | `SD_EJECTION_DETECTED` then `SD_EJECTED` | The shell switches to the fatal SD-ejected screen; a reboot (with media restored) recovers. |
| Settings corruption | `SETTINGS_RESET` | Rewrite defaults and continue boot; never brick on a bad settings file. |
| Settings write interrupted | `SETTINGS_RECOVERED` | Promote `/data/settings.json.tmp` to the main file on next boot. |

## Out-of-scope items

- `OTA`
- `LVGL redesign`
- `multi-VM`
- `multi-foreground-app`
