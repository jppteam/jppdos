# ESP-IDF Native/App Contract

This document defines the native/app boundary for JPPDOS. The native ESP-IDF core in `components/jpp_core/` owns boot sequencing, storage, settings, broker policy, hardware drivers, and the UI/runtime bridge. Apps interact with the system exclusively through the App SDK and capability-gated broker services.

## Boot order

1. Storage initializes, creating the base writable layout for `/data` and `/lib` before any settings or UI work.
2. Settings load from `/data/settings.json` under schema v2 rules. If only `/data/settings.json.tmp` exists, it is promoted and `SETTINGS_RECOVERED` is emitted.
3. `/sd` mount is attempted before app discovery.
4. Runtime path wiring for `/lib` and `/sd/lib` happens before any MicroPython app imports.
5. Boot mode is `normal` only when `/sd` is mounted and recovery is not forced. Otherwise boot mode is `recovery`.
6. The native broker initializes OLED, RTC, SD, network, and keypad services before discovery.
7. App discovery runs after broker init. Recovery mode exposes only built-in/system availability; normal mode includes enabled apps plus disabled and rejected accounting.
8. `SYSTEM_READY` is emitted before launcher handoff.

## Settings v2

- `schema_version` is `2`.
- Settings live at `/data/settings.json`, with `/data/settings.json.tmp` as the recovery file.
- Schema `1` payloads are accepted, normalized, rewritten as schema `2`, and logged as `SETTINGS_MIGRATED`.
- Corrupt or non-dict payloads trigger `SETTINGS_RESET`, then the defaults are rewritten and boot continues.
- Broker-mediated settings mutations are immediate-write operations.
- The persisted JSON file is authoritative. Any ESP-IDF NVS use is auxiliary only and does not replace `/data/settings.json` as the source of truth.
- Temp-file recovery keeps the last complete JSON payload available by promoting `/data/settings.json.tmp` when it is the only settings file present.

## Logical paths

| Logical path | Owner | Contract |
| --- | --- | --- |
| `/data` | Native storage layer | Flash-backed writable state for settings, crash logs, and app-private data. Maps to the ESP-IDF data filesystem mount. |
| `/lib` | Native runtime layer | System-owned shared library/runtime path. Maps to the ESP-IDF runtime filesystem mount. |
| `/sd` | Removable media layer | Removable app/media root; absence forces recovery-compatible behavior. A distinct removable mount, never a flash alias. |
| `/sd/apps/<dir>` | App packaging | Manifest-driven app package root. Packages are discovered only under this subtree. |
| `/sd/lib` | Runtime layer | Optional shared SD library root; system-owned, not a promise of unrestricted imports. |
| `/sd/apps/<app_id>/` | App SDK (`files.scoped`) | App-private writable subtree on SD. `App SDK file_read()` / `file_write()` / `file_list()` paths are rooted here. |
| `/sd/shared/<app_id>/` | App SDK (`files.shared`) | Per-app subdirectory within the shared SD area. Accessible to any app holding `files.shared`. |

### ESP-IDF mount contract

- `data_fs` is the flash-backed ESP-IDF VFS mount that backs `/data`.
- `runtime_fs` is the flash-backed ESP-IDF VFS mount that backs `/lib`.
- `/sd` is mounted independently from removable media and is never folded into either flash mount.

### Path validation rules

- Manifest and SDK-relative package paths must be relative, must not start with `/`, and must not contain `..` traversal segments.
- Package entry paths are resolved relative to `/sd/apps/<dir>` and must stay within the app root.
- `files.scoped` SDK paths are rooted at `/sd/apps/<app_id>/` and accessed only through the broker/SDK.
- `files.shared` SDK paths are rooted at `/sd/shared/<app_id>/` and accessed only through the broker/SDK.
- `files.full` SDK paths must be absolute under `/sd/` with no `..` traversal; each path requires per-open user approval.
- Any path outside these rooted logical spaces is rejected rather than normalized into a broader filesystem escape.

## Broker ownership

| Resource | Native owner | App-visible path |
| --- | --- | --- |
| OLED + shared I2C bus | Broker serializes access and owns rendering | Apps update text through SDK frame APIs, not driver handles. |
| RTC/time, network, battery | Broker owns time reads, network state, and ADC reads | `device.status` capability gates `App SDK device_status()`, which returns time, network, battery, and SD presence in one call. |
| Files on `/sd` (scoped) | Broker validates logical paths and mediates reads/writes | `files.scoped` gates `App SDK file_read()` / `file_write()` / `file_list()` rooted at `/sd/apps/<app_id>/`. |
| Files on `/sd` (shared) | Broker validates logical paths and mediates reads/writes | `files.shared` gates `App SDK shared_read()` / `shared_write()` / `shared_list()` rooted at `/sd/shared/<app_id>/`. |
| Files on `/sd` (full) | Broker validates absolute paths; each open requires user approval | `files.full` gates `App SDK file_open()` / `handle_*()`. Handles are valid for the app session. |
| Keypad + launcher focus | Launcher/system UI owns input polling | Apps receive normalized actions and lifecycle callbacks, not raw keypad driver access. |
| Recovery/settings policy | Native core + Settings UI only | Runtime apps do not mutate recovery policy directly. |

## SSD1306 text UI

- The launcher is a text-first SSD1306 shell with a root screen, a built-in Settings entry, line-based rendering, app-open / app-unavailable dialogs, and crash recovery back to the launcher.
- Rendering is line-based and satisfies an eight-line frame contract.
- Settings is a built-in system UI entry available in both normal and recovery mode.
- App load failures show an unavailable dialog; the shell does not crash.
- Foreground callback crashes write `/data/ui_crash.log`, reset the stack to the launcher root, and show a crash dialog before normal launcher flow resumes.

## Single shared VM

- Exactly one embedded VM exists for MicroPython apps.
- The native core never calls app objects from arbitrary RTOS tasks; foreground hooks, idle hooks, and background callbacks are all marshaled onto the shared VM task.
- Exactly one app may own the foreground at a time.
- Background work is opt-in: it only runs when the manifest enables background mode and the app registers tasks through the SDK.
- Built-in system UI flows such as Settings are native/system-owned and do not run inside the shared VM.

## Resource budget slice

The budget slice is defined in `components/jpp_core/include/jpp_resource_budget.h` and is enforced by host probes until ESP-IDF target telemetry exists.

| Area | Target | Limit / floor | Failure behavior |
| --- | --- | --- | --- |
| VM heap | `131072` bytes | reject configs above `196608` bytes | `jpp_vm_runtime_prepare()` returns `INVALID_CONFIG`. |
| VM stack | `8192` words | reject configs above `12288` words | `jpp_vm_runtime_prepare()` returns `INVALID_CONFIG`. |
| VM queue | `4` requests | reject configs above `8` requests | Oversubscription returns `QUEUE_FULL`; over-limit config returns `INVALID_CONFIG`. |
| Native free heap after boot | floor tracked at `65536` bytes | no target probe yet | Later ESP-IDF telemetry must fail below the floor. |
| SDK frame bridge | `7` lines | `7` lines | Extra frame lines return `TEXT_TRUNCATED`. |
| SDK background tasks | `2` registered tasks | `4` registered tasks | Further registration returns `TASK_LIMIT`. |
| SDK file handles (`files.full`) | `2` open handles | `4` open handles | Further `file_open()` calls return `HANDLE_LIMIT`. |
| SDK BLE connections (`ble.connect`) | `1` open connection | `2` open connections | Further `ble_connect()` calls return `BLE_CONN_LIMIT`. |
| Broker result fields | `4` fields | `8` fields | Further fields return `FIELD_OVERFLOW`. |
| Broker locks | `4` named locks | `8` named locks | Further resources return `FIELD_OVERFLOW`. Named locks in use: `storage`, `device`, `vm_queue`, `ble_radio`, `ble_client`, `ble_host`. |

## Best-effort sandbox

- Runtime apps have a restricted import surface. The native bridge may expose explicit SDK modules without opening raw hardware modules.
- Apps do not get direct hardware, unrestricted OS, raw network, or arbitrary filesystem access.
- Capability checks are mandatory inside the shared VM. The broker is the final authority on file, time, and network access.
- App-private file access is rooted at `/sd/apps/<app_id>/` (`files.scoped`) or `/sd/shared/<app_id>/` (`files.shared`). Arbitrary SD access (`files.full`) requires per-path user approval at open time.
- This is a best-effort sandbox for product stability and policy enforcement, not a malicious-code containment boundary.

## Manifest v2 package model

App packages are discovered from `/sd/apps/<dir>/manifest.json`. Each package must include a compiled entry module and a valid manifest.

| Field | Type | Rule |
| --- | --- | --- |
| `schema_version` | int | Must be `2`. |
| `app_id` | string | Stable unique app identifier; must not collide with built-in IDs such as `settings`. |
| `name` | string | Launcher label. |
| `version` | string | App package version string. |
| `sdk_min` | int | Inclusive minimum supported SDK version. |
| `sdk_max` | int | Inclusive maximum supported SDK version. |
| `entry` | string | Relative compiled entry path inside the app root; no leading `/` and no `..` traversal. |
| `capabilities` | list[string] | Tier 0 (auto): `files.scoped`, `files.shared`, `device.status`. Tier 1 (one-time grant): `ipc.send`, `http.request`, `device.kv`, `ble.scan`, `ble.advertise`. Tier 2 (per-session grant): `files.full`, `network.bind`, `ble.connect`, `ble.host`. |
| `background.enabled` | bool | Enables or disables background execution for the app. |
| `background.mode` | string | Must be `serialized`; background callbacks run on the shared VM task, never concurrently. |
| `modules` | list[string] | Optional relative compiled support module paths shipped beside the entry module. |
| `toolchain.runtime_version` | string | Exact runtime version pinned for this release. |
| `toolchain.cross_version` | string | Exact compiler version pinned for this release. |
| `toolchain.bytecode_abi` | int | Bytecode ABI version; mismatch rejects the app before launch. |

- `entry` and every `modules` item are relative package paths; leading `/` and `..` traversal are rejected.
- `manifest.json` is the discovery source of truth for the app directory.
- `entry_path` is rooted under `/sd/apps/<dir>` and derived from the manifest, not provided as an absolute path.
- App entrypoints are compiled modules. Uncompiled source files are not loaded.
- Packages that omit compatibility metadata are rejected.

## SDK surface and versioning

- The native bridge exports SDK version `1`. The `sdk_min` / `sdk_max` range check is inclusive.
- The app-facing App SDK surface is: `set_frame()`, `request_close()`, `log()`, `dialog()`, `list()`, `confirm()`, `input()`, `poll_key()`, `wait_key()`, `canvas_write()`, `canvas_clear()`, `canvas_draw_pixel()`, `device_status()`, `get_time()`, `file_read()`, `file_write()`, `file_list()`, `shared_read()`, `shared_write()`, `shared_list()`, `file_open()`, `handle_read()`, `handle_write()`, `handle_list()`, `handle_close()`, `add_background_task()`, `http_request()`, `ipc_send()`, `ipc_recv()`, `kv_get()`, `kv_set()`, `kv_delete()`, `ble_scan()`, `ble_advertise_start()`, `ble_advertise_stop()`, `ble_connect()`, `ble_read_char()`, `ble_write_char()`, `ble_disconnect()`, `ble_service_register()`, and `ble_service_unregister()`. The `dialog()`, `list()`, `confirm()`, and `input()` helpers are the standard high-level UI prompts (message box, selection list, Deny/Allow consent prompt, and on-screen text/value entry) and require no capability. Modal helpers with a title draw the shared "signature line" rule on page 1 (matching the launcher/settings/WebDAV header). `confirm()` is the single consent surface shared by capability and file-access prompts. `input()` with `INPUT_DATE` / `INPUT_TIME` renders a field spinner (LEFT/RIGHT pick field, UP/DOWN adjust) with `123` (masked manual entry), `now` (RTC), Cancel, and OK buttons; it returns `YYYY-MM-DD` for dates and `HH:MM:SS` (seconds included) for times.
- `device_status()` (capability `device.status`) returns `battery_pct` and `charging`; `get_time()` returns `"YYYY-MM-DD HH:mm"` and is also gated by `device.status`. `kv_get()` returns a non-OK status when the key is absent (so a get after a delete reports "not found" rather than an empty value). `log()` writes the event to the serial console under tag `app_log`.
- `file_read()` / `file_write()` / `file_list()` use app-local relative-path semantics rooted at `/sd/apps/<app_id>/`.
- `shared_read()` / `shared_write()` / `shared_list()` use app-local relative-path semantics rooted at `/sd/shared/<app_id>/`.
- `file_open()` takes an absolute `/sd/` path, prompts the user for approval, and returns a session-scoped handle.
- Adding a new privileged surface or changing an existing method's behavior in a breaking way requires an SDK version bump and manifest range validation.

## App lifecycle

- App discovery classifies apps as enabled, disabled, or rejected.
- MicroPython apps export `create_app(sdk)` — the MicroPython runner calls it once and dispatches lifecycle hooks (`on_start`, `on_stop`, `on_foreground`, `on_background`, `on_idle`, `handle_action`) on the returned object.
- Native apps export `void jpp_app_entry(jpp_sdk_context_t *ctx)` — the ELF loader (`jpp_native_loader_core`) resolves and calls it directly; the function runs synchronously with no separate lifecycle hooks.
- `BACK` requests close. Background-enabled sessions may remain resident only while registered background tasks exist and stay within watchdog limits.
- Opening an app that already has a live session foregrounds the existing session instead of starting a second copy.

## Failure taxonomy

| Failure class | Marker / code | Required response |
| --- | --- | --- |
| Bad manifest | `APP_REJECTED` with `INVALID_JSON`, `INVALID_MANIFEST`, `SCHEMA_MISMATCH`, `INVALID_ENTRY`, `SDK_MISMATCH`, or `DUPLICATE_APP_ID` | Reject only the bad app, count it in `rejected`, keep booting, and still reach the launcher. |
| Missing entry payload | `APP_REJECTED` with `ENTRY_MISSING` | Reject the package before launch and keep the rest of the catalog available. |
| Corrupt entry payload | `APP_REJECTED` with `ENTRY_CORRUPT` when preflight catches it, otherwise `APP_RUNTIME_ERROR` on open | Keep the launcher alive; the app becomes unavailable rather than taking down the shell. |
| Runtime version mismatch | `APP_REJECTED` with `RUNTIME_MISMATCH` | Reject the app before launch if runtime/toolchain metadata or bytecode ABI does not match the pinned runtime. |
| Unsupported import | `APP_RUNTIME_ERROR` with `UNSUPPORTED_IMPORT` | Abort app launch or stop the session, then return control to the launcher without escalating privileges. |
| `create_app` failure | `APP_RUNTIME_ERROR` with `CREATE_APP_FAILED` | Show app unavailable UI, do not create a live session, and keep the launcher responsive. |
| Callback exception | `APP_CRASH` for foreground/idle failures or `APP_BG_ERROR` for background failures | Foreground/idle failures write `/data/ui_crash.log` and reset to the launcher; background failures stop the session and log the failure. |
| Broker denial | `ACCESS_DENIED` | Return an error result to the app, log the denial, and do not grant the privileged action. |
| SD removal during runtime | `SD_REMOVED` | Close the affected app session, refresh discovery, and keep the system in launcher/recovery-compatible state. |
| Storage full | `STORAGE_FULL` | Preserve the last good `/data/settings.json`; fail the specific write request without silently truncating or partially replacing data. |

## Out-of-scope items

- `OTA`
- `LVGL redesign`
- `multi-VM`
- `multi-foreground-app`
