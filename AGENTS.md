# PROJECT KNOWLEDGE BASE

## DOCUMENTATION MAINTENANCE (NON-NEGOTIABLE)
These project documents — every `AGENTS.md`, `README.md`, and `DEVELOPMENT.md` —
**MUST** be updated in the same change as any code or behaviour they describe.
This is non-negotiable and non-deferrable: if you change the App SDK surface,
broker policy, capability tiers, manifest schema, settings schema, boot
behaviour, hardware mapping, or any other documented contract, update the
relevant docs in the same commit. Out-of-date documentation is treated as a
defect, not a follow-up.

## OVERVIEW
JPPDOS is an ESP-IDF C/C++ firmware repo for ESP32-C6-class hardware. Native boot, storage, settings, broker policy, UI, and hardware services live in `components/jpp_core/`; the build and flash flow is container-friendly and centered on `idf.py` plus Docker.

## STRUCTURE
```text
jppdos/
├── components/jpp_core/   # native core implementation
├── main/                  # ESP-IDF app entrypoint + settings screen
├── apps/                  # example/reference app packages (BounceJPP, meetapp, testapp_native, testapp_mp)
├── scripts/               # flash and helper scripts
├── tests/                 # host-side checks and fixtures
└── wokwi/                 # simulator assets and reference topology
```

## WHERE TO LOOK
| Task | Location | Notes |
|---|---|---|
| Core boot and services | `components/jpp_core/` | startup, settings, broker, storage, UI, and device policy |
| Build flow | `idf.py`, Dockerfiles/scripts | native build, target selection, and reproducible container runs |
| Flashing | `scripts/flash.sh` | board flashing and serial handoff |
| Storage / settings | `components/jpp_core/` | `/data` state, schema migration, temp-file recovery; app data roots are `/sd/apps/<app_id>/` (scoped) and `/sd/shared/<app_id>/` (shared) |
| Broker policy | `components/jpp_core/` | capability checks, exclusive access, and service gating |
| Boot entry point | `main/app_main.c` | boot sequencer, keypad task, UI render loop, power management, SD ejection |
| Hardware bring-up | `main/jpp_hw_init.c/.h` | I²C init, flash/SD mount |
| Settings screen | `main/jpp_settings_screen.c/.h` | full settings UI (Shutdown/Reboot, Wi-Fi, Time, Sleep timers, Sound, SD card, Backup settings, Factory Reset, **\* Device Info \*** (hidden unless LRV data present), **User's name**, About) |
| Settings file helpers | `main/jpp_settings_load.c/.h` | file_exists, probe/write/read settings |
| Boot display | `main/jpp_boot_display.c/.h` | splash screen and progress steps |
| Wi-Fi init | `main/jpp_wifi_init.c/.h` | NVS + esp_wifi STA mode setup |
| Native service callbacks | `main/jpp_native_services.c/.h` | file I/O, HTTP, KV, RTC, SD-ejection flag, path-prompt callbacks |
| App lifecycle | `main/jpp_app_dispatch.c/.h` | app discovery, consent, launch, teardown; lazy consent via `apply_consent()` + `jpp_sdk_set_pending_caps()`; `sd_app_task_fn` sets `close_requested` on exit so the main loop can tear down |
| Serial manager | `main/jpp_serial_mgr.c/.h` | JPPD-SMP binary protocol over UART0: SD file management, device-info queries, and LRV data retrieval from a host PC; requires user consent before session opens |
| LRV data | `main/jpp_lrv.c/.h` | Limited Run Verification NVS storage and crypto: encrypt/decrypt blob in `jpp_lrv` namespace, sign challenges with device Ed25519 key, supply display info and full data for the verification server |
| LRV server | `main/jpp_lrv_server.c/.h` | HTTP verification server on port 3000; serves certificate hex dump and challenge-response; "Open Certificate Page" is a direct link to `https://jppdevice.com/lrv?ts=...&name=...&serial=...&resp=...`; mutually exclusive with WebDAV; requires LRV data to be unlocked |
| Wokwi reference | `wokwi/` | simulator topology only, not hardware sign-off |
| Host-side tests | `tests/` | architecture checks, contract checks, manifest validation, and fixtures |

## CODE MAP
| Symbol | Type | Location | Role |
|---|---|---|---|
| `jpp_boot_core` | component | `components/jpp_core/` | boot ordering, readiness, recovery decisions |
| `jpp_settings_core` | component | `components/jpp_core/` | settings schema, normalization, recovery |
| `jpp_broker_core` | component | `components/jpp_core/` | capability gate and exclusive resource access |
| `jpp_vm_core` | component | `components/jpp_core/` | shared VM scheduling and runtime isolation |
| `jpp_sdk_bridge` | component | `components/jpp_core/` | App SDK surface: frame, file I/O, buzzer, wakelock, dialog/list/confirm/input/file-pick UI helpers. `jpp_sdk_confirm()` is the shared Deny/Allow consent surface (used by capability + `files.full` path prompts). Titled modals draw the signature-line rule on page 1 (`frame_title_rule`). `jpp_sdk_input` with `INPUT_DATE`/`INPUT_TIME` is a field spinner (LEFT/RIGHT field, UP/DOWN value) with 123/now/Cancel/OK buttons; returns `YYYY-MM-DD` / `HH:MM:SS`. `jpp_sdk_kv_get` returns non-OK when a key is absent. |
| `jpp_native_loader_core` | component | `components/jpp_native_loader_core/` | ELF32/RISC-V PIC loader for `app_type "native"` binaries |
| `jpp_app_pool` | component | `components/jpp_app_pool/` | single shared static `.bss` pool (`JPP_APP_POOL_BYTES`, 64 KB) for the running app: executable code for native apps **or** the MicroPython GC heap. `jpp_app_pool_acquire()`/`_release()`/`_in_use()`. Native and MP apps are mutually exclusive, so one pool serves both — merging the former 64 KB exec + 32 KB GC pools returned ~32 KB to the general heap. Leaf component (REQUIRES only `log`) to avoid a cycle: `jpp_native_loader_core` and `jpp_core` both depend on it. |
| `jpp_ui_core` | component | `components/jpp_core/` | launcher shell, WebDAV server screen, dialog/crash screens, power state tracking; `jpp_ui_shell_clear_sd_apps(shell)` removes all non-builtin apps from the catalog (clamps cursor) — called by background discovery before applying a fresh scan result |
| `jpp_rtc_core` | component | `components/jpp_core/` | DS1307 I²C driver, datetime state, software-tick live time |
| `jpp_buzzer_core` | component | `components/jpp_core/` | LEDC buzzer driver; predefined sounds + custom tone/sequence API. `jpp_buzzer_set_volume(percent)` / `jpp_buzzer_get_volume()` — volume is controlled via GPIO drive capability (`GPIO_DRIVE_CAP_0`–`3`), not duty cycle; duty stays fixed at 50% (`JPP_HW_BUZZER_DUTY`) for all non-zero levels so waveform quality is unchanged. 0% mutes by setting duty to 0. Default after init is 100% (CAP_3). `load_buzzer_volume()` in `app_main.c` applies the persisted level before the startup chime. `jpp_startup_jingle_t` enum (0–10): DEFAULT, WINXP, WIN31, MAC, RICKROLL, NOKIA_ON, NOKIA_TUNE, SANDSTORM, DOOM, CLUTTERFUNK, OFF. `jpp_buzzer_play_startup_jingle(jingle)` plays the chosen jingle (no-op for OFF). `jpp_startup_jingle_name(jingle)` returns the display string. Playback comes in **blocking** (`jpp_buzzer_tone`/`_play_sequence`/`_play`/`_play_startup_jingle`) and **async** (`jpp_buzzer_play_sequence_async`/`_play_async`/`_play_startup_jingle_async`) forms: async copies the sequence into an inbox and hands it to a dedicated static-allocated player task, returning immediately. Submitting a new async sequence — or calling `jpp_buzzer_stop()` — preempts whatever is playing (generation counter checked between notes), so cycling previews cut the previous one off within one note. Blocking `_play_sequence` also preempts any async sequence so the two never drive the LEDC channel at once. Settings jingle previews and the boot chime use the async form. |
| `jpp_heap_monitor` | component | `components/jpp_core/` | global heap-pressure diagnostics: `jpp_heap_monitor_init()` (call once early in `app_main`) registers a `heap_caps_register_failed_alloc_callback` that logs the size/caps/function of any failed malloc (turns cryptic `wifi:m f ...` into an attributable `ALLOC FAILED` line) and starts a low-priority sampler task that WARNs below `JPP_HEAP_MON_WARN_BYTES` (30 KB) / ERRORs below `JPP_HEAP_MON_CRIT_BYTES` (15 KB) with hysteresis + a `heap_caps_print_heap_info` dump on first entry to CRIT. `jpp_heap_monitor_log(label)` emits a one-line `heap @label` marker for manual checkpoints (used by `jpp_fileserver_core` at WebDAV start/stop). |
| `jpp_resource_budget` | header-only | `components/jpp_core/include/jpp_resource_budget.h` | runtime and broker budget limits (compile-time `#define` constants only) |
| `jpp_string_util` | header-only | `components/jpp_core/include/jpp_string_util.h` | shared string helpers |
| `jpp_fileserver_result_t` | enum | `components/jpp_core/include/jpp_fileserver_core.h` | result codes for `jpp_fileserver_*`; `jpp_fileserver_status_t` carries `ip`, `port`, and `password` (up to `JPP_FILESERVER_PASS_MAX` chars; random `JPP_FILESERVER_PASS_LEN`-char or user-supplied static); use `jpp_fileserver_start_with_password()` for static passwords |
| `app_main` | entrypoint | `main/app_main.c` | boot sequencer (steps 1–8), keypad task, UI render loop, power mgmt, SD ejection; dispatches `JPP_VM_REQUEST_IDLE` every `JPP_UI_REFRESH_MS` and `JPP_VM_REQUEST_ACTION` (with `app_id`) from the keypad task to running MicroPython apps |
| `jpp_hw_init` | module | `main/jpp_hw_init.c/.h` | `init_i2c()`, `mount_flash_storage()`, `mount_sd()` |
| `jpp_settings_screen` | module | `main/jpp_settings_screen.c/.h` | settings UI rendered directly to SSD1306; sections: Shutdown/Reboot, Wi-Fi, Time, Sleep timers, **Sound** (Volume, Jingle, Test — 3 rows; LEFT/RIGHT on Volume cycles level, LEFT/RIGHT on Jingle cycles startup jingle and plays a preview, OK on Test plays the selected jingle), SD card, Backup settings, Factory Reset, **\* Device Info \*** (hidden unless LRV data present), **User's name** (text input, persisted in NVS `jpp_user`/`username`, max `JPP_SETTINGS_USERNAME_MAX` = 64 chars), About; section visibility controlled by `section_is_visible()` |
| `jpp_settings_load` | module | `main/jpp_settings_load.c/.h` | `file_exists()`, `probe_settings_payload()`, `write_settings()`, `read_force_recovery()` |
| `jpp_boot_display` | module | `main/jpp_boot_display.c/.h` | `boot_disp_show_splash()`, `boot_disp_step()` |
| `jpp_wifi_init` | module | `main/jpp_wifi_init.c/.h` | `init_wifi()`, `wifi_connect()`, `wifi_disconnect()`, `wifi_is_connected()`, `wifi_get_connected_ssid()`, `wifi_get_saved_ssid()`, `wifi_is_connecting()`, `wifi_ensure_started()`; auto-reconnect capped at `WIFI_MAX_RECONNECT_ATTEMPTS` (10) — `wifi_is_connecting()` returns false once the limit is hit; call `wifi_disconnect()` to abort the loop early |
| `jpp_native_services` | module | `main/jpp_native_services.c/.h` | `jpp_native_services_init()`, all I/O and BLE callbacks, `s_sd_ejection_detected`. Provides the `device.status` callback (battery_pct/charging — call `jpp_native_services_set_battery_state()` from the main loop), the `log_writer` callback (serial tag `app_log`), and the RTC reader (static result buffer — broker stores pointers, not copies). The `path_prompt` callback renders via `jpp_sdk_confirm`. |
| `jpp_app_dispatch` | module | `main/jpp_app_dispatch.c/.h` | `discover_apps()`, `launch_sd_app()`, `teardown_sd_app()`, consent machinery; `apply_consent()` partitions declared caps into immediately-granted and pending; `jpp_app_consent_prompt()` (public) wraps `prompt_permission()` + `grant_persist()` for lazy per-use prompts; grants persisted in `/data/grants/<app_id>.json`. Background discovery: `discover_apps_background_start(normal_mode)` spawns a one-shot FreeRTOS task that scans `/sd/apps` into a static buffer and sets a volatile ready flag; `discover_apps_background_ready()` polls that flag (non-blocking); `discover_apps_apply_to_shell(shell)` clears SD entries via `jpp_ui_shell_clear_sd_apps()` then re-adds the buffered results. The main loop triggers a fresh scan each time `top_screen` transitions to `"launcher"` (skipped on `ui_tick == 0` to avoid a duplicate boot scan). |
| `jpp_file_picker` | module | `main/jpp_file_picker.c/.h` | firmware file browser: `jpp_file_picker()` blocks on SSD1306 + `s_action_queue`; dirs have "/" suffix, ".." navigates up, long names scroll as marquees; SDK counterpart is `jpp_sdk_file_pick()` |
| `jpp_serial_mgr` | module | `main/jpp_serial_mgr.c/.h` | JPPD-SMP v1 implementation: `jpp_serial_mgr_init()` installs the UART0 RX driver and wraps the ESP_LOG vprintf sink with a TX mutex; `smp_rx_task` scans for the 4-byte SOF (`\x01JPP`), verifies CRC-16/CCITT-FALSE, dispatches to command handlers. `jpp_serial_mgr_set_rtc(rtc)` provides the RTC pointer for timestamp generation (call after `jpp_serial_mgr_init()`). `jpp_serial_mgr_needs_render()` / `handle_action()` / `render()` integrate with the main-loop UI cycle (consent dialog + active-session screen). SD app launch is blocked while a session is open. See JPPD-SMP WIRE FORMAT section below for the protocol specification. |
| `jpp_lrv` | module | `main/jpp_lrv.c/.h` | LRV NVS management and crypto: `jpp_lrv_has_data()`, `jpp_lrv_is_unlocked()`, `jpp_lrv_unlock(password)`, `jpp_lrv_get_display_info(serial, pubkey_str[16])`, `jpp_lrv_get_full_data()`, `jpp_lrv_sign_challenge()`, `jpp_lrv_get_encrypted_blob()` / `_store_encrypted_blob()`. Uses libsodium `crypto_secretbox_easy` (key = BLAKE2b-256(password)) for the encrypted blob. Blob plaintext layout: `serial(2) + device_pubkey(32) + device_seckey(64) + cert_sig(64) + hwid(24) + cert_len(2) + cert(N)`; `run_size`, `device_type`, and `mfr_pubkey` are embedded in the certificate text only and are NOT separate NVS or blob fields. |
| `jpp_lrv_server` | module | `main/jpp_lrv_server.c/.h` | `jpp_lrv_server_start(rtc)` / `_stop()` / `_is_running()` / `_get_addr()`; port 3000; single handler `GET /` (wildcard): displays cert, cert_sig, device_pubkey, challenge (`{username}\|{iso8601}`), resp_sig; "Open Certificate Page" is a direct `<a href>` to `https://jppdevice.com/lrv?ts=...&name=...&serial=...&resp=...` (no server-side redirect). No custom challenge input. Checks `jpp_fileserver_get_status()` for WebDAV mutual exclusion. |

## CONVENTIONS
- `components/jpp_core/` is the implementation source of truth for reusable firmware components.
- `main/` hosts firmware-specific modules that drive hardware directly (SSD1306, settings screen).
- Use `idf.py set-target esp32c6`, `idf.py build`, and `idf.py flash` for native workflow.
- Prefer Docker-based commands for reproducible builds and environment parity.
- Keep hardware-facing docs aligned with the repo's reference pin map and Wokwi topology.
- Settings live under `/data/settings.json` and should preserve schema migration and recovery behavior.
- All public jpp_core headers use `#pragma once` (no `#ifndef` guards).
- Error return types: use `jpp_<module>_result_t` or `jpp_<module>_status_t` enums; never `esp_err_t` on the jpp_core public API surface.
- Shared string utilities (`jpp_str_eq`, `jpp_str_nonempty`, `jpp_str_copy`) live in `jpp_string_util.h`; do not duplicate them per-module.
- Named constants for all hardware defaults: battery in `jpp_battery_core.h`, keypad in `jpp_keypad_core.h`, OLED in `jpp_oled_core.h`, RTC in `jpp_rtc_core.h`, ADC resolution in `jpp_hw_config.h`, buzzer/DS1307 in `jpp_hw_config.h`.
- Use `NULL` for pointer comparisons, not `0`.
- App SDK `set_frame(lines, count)` — no footer parameter.
- App SDK `jpp_sdk_file_pick(context, out_path, out_path_len, out_result)` — requires `files.full`; browses `/sd` from root, ".." to go up, "/" suffix on dirs, marquee for long names; firmware counterpart is `jpp_file_picker()` in `main/`.
- Backup settings: `Settings > Backup settings` — "Backup to SD card" writes `/sd/backups/settings_YYYYMMDD_HHMMSS.json` (NVS + settings.json); "Restore from file" invokes `jpp_file_picker`, parses backup JSON, restores NVS namespaces (`jpp_time`, `jpp_power`, `jpp_webdav`, `jpp_sound`, `jpp_lrv`, `jpp_user`) and settings.json, then restarts. LRV data in backups is always encrypted (re-encrypted from cached password if data was unlocked on device).
- User's name: persisted in NVS namespace `jpp_user`, key `username` (string, max `JPP_SETTINGS_USERNAME_MAX` = 64 chars). Loaded at boot in `load_username()` (called from `run_main_loop()` after NVS init). Edited via `Settings > User's name` text input. Used as the subject in LRV challenges (`{username}|{iso8601}`) and as the `name=` parameter in the verification URL.
- LRV: `Settings > * Device Info *` section appears when `jpp_lrv` namespace contains either a locked (`lrv_enc` blob) or unlocked (`lrv_serial` key) entry. Locked data requires the printed password to unlock; after unlock the decrypted fields and password are persisted so future backups can re-encrypt. Pressing OK in the MAIN subscreen logs the certificate, cert_sig, device_pubkey, challenge (`{username}|{iso8601}`), and resp_sig (all as separate WARN lines) and starts the HTTP verification server on port 3000 (mutually exclusive with WebDAV). Challenge format: `{username}|{YYYY-MM-DDTHH:MM:SS}` (username from NVS `jpp_user/username`). Manufacturing backup files are generated by `scripts/lrv_manufacturing.py`. The `hwid` field in the certificate is the device's eFuse MAC obtained via `esp_efuse_mac_get_default()`. The NVS blob does NOT store `mfr_pubkey`, `run_size`, or `device_type` as separate fields — they are encoded in the certificate text and parsed from there.
- Buzzer volume: persisted in NVS namespace `jpp_sound`, key `buzzer_vol` (u8). Discrete steps: 0 / 25 / 50 / 75 / 100. Implemented via `gpio_set_drive_capability()` (CAP_0–3 maps to 25–100%); duty stays fixed at 50% so tone quality is constant across levels. 0% mutes by zeroing LEDC duty. Loaded and applied before the startup chime (`load_buzzer_volume()` in `app_main.c`, after `nvs_flash_init`). Changed at runtime via `settings_do_volume_change()`. Settings > Sound: LEFT/RIGHT on Volume cycles level (plays CLICK at new level); LEFT/RIGHT on Jingle cycles startup jingle and plays a preview; OK on Test plays the selected startup jingle.
- Startup jingle: persisted in NVS namespace `jpp_sound`, key `startup_jingle` (u8, `jpp_startup_jingle_t`). Loaded alongside `buzzer_vol` in `load_buzzer_volume()`. Changed at runtime via `settings_do_jingle_change()`. The boot startup sound in `app_main.c` calls `jpp_buzzer_play_startup_jingle_async(s_startup_jingle)` (async, so the launcher comes up while it plays) instead of the fixed `JPP_BUZZER_SOUND_STARTUP`. Settings > Sound jingle previews (LEFT/RIGHT cycle, OK on Test) also use the async form, so the UI never blocks for the jingle and a new selection cuts the previous preview off. Jingles: DEFAULT (original chime), WinXP Startup, Win3.1 Startup, Mac128k, Rick Roll, Nokia Power On, Nokia Tune, Sandstorm, DOOM, Clutterfunk, OFF. The non-default melodies are RTTTL transcriptions (kept faithful to the source d/o/b headers; repeated-note jingles like Sandstorm carve a small rest out of each note so the stutter re-attacks).
- Firmware version string: `JPPDOS_VERSION` in `main/jpp_settings_screen.h`.
- Capability consent is **lazy / per-use**: `apply_consent()` in `main/jpp_app_dispatch.c` only grants caps at launch for tier-0 and already-persisted tier-1; all other declared caps go into the **pending set** (`jpp_sdk_set_pending_caps()`). The prompt fires the first time an SDK call requires that cap via `jpp_sdk_ensure_cap()` → `consent_prompt_cb` → `jpp_app_consent_prompt()` → `prompt_permission()`. Tier rules: tier-0 (`files.scoped`, `files.shared`, `device.status`) auto-granted; tier-1 (`ipc.send`, `http.request`, `device.kv`, `ble.scan`, `ble.advertise`) prompts once then persists to `/data/grants/<app_id>.json`; tier-2 (everything else: `files.full`, `network.bind`, `ble.connect`, `ble.host`) prompts on first use every launch, never persisted. A denied cap remains out of the broker caller list — the broker enforces the gate, and `jpp_sdk_ensure_cap()` logs every denial (user-declined, not-declared-in-manifest, or grant-overflow) under tag `jpp_sdk`. `prompt_permission()` builds the request lines and delegates to `jpp_sdk_confirm()`, which renders via `jpp_sdk_set_frame` + `jpp_sdk_wait_key` (runs from the app task; must NOT use SSD1306 directly or `s_action_queue`). An `-Wunused-function` warning on `prompt_permission` indicates `jpp_app_consent_prompt()` has been broken. NOTE: a manifest may declare up to `SD_MANIFEST_CAP_MAX` (16) capabilities, which must stay `<= JPP_SDK_PENDING_CAP_MAX` — a smaller limit silently truncates the manifest list and drops the trailing caps so they can never be granted. When `jpp_sdk_ensure_cap()` grants a pending cap it appends to the caller list pointing at the stable pending-slot string and leaves the pending entry in place (the caller-list check short-circuits future lookups); it must never compact the pending array in a way that mutates a buffer the caller list points at.

## ANTI-PATTERNS (THIS PROJECT)
- Do not bypass the service broker for file, network, keypad, RTC, or storage access.
- Do not treat Wokwi success as hardware sign-off for ADC, RTC, power-loss, Wi-Fi, or SD reliability.
- Do not invent a second architecture path outside the ESP-IDF native core.
- Do not add package-manager or monorepo guidance; this repo is firmware-first.
- Do not put firmware-layer code (SSD1306 direct calls, settings screen) in `jpp_core/` — it belongs in `main/`.
- Do not make `apply_consent()` auto-grant capabilities without user confirmation. The interactive `prompt_permission()` dialog is intentional security UX — replacing it with unconditional grants silently removes the permission model and breaks the broker's trust boundary. Any refactor of `apply_consent()` must keep the tier-0/tier-1/tier-2 rules intact (see CONVENTIONS above).

## BUILTIN APPS
| App ID | Name | Notes |
|---|---|---|
| `settings` | Settings | Full settings screen; Shutdown/Reboot, Wi-Fi, Time, Sleep timers, Sound (buzzer volume 0/25/50/75/100% + startup test), SD card, Backup settings, Factory Reset, \* Device Info \* (LRV only), User's name, About |
| `webdav` | WebDAV server | WebDAV file transfer screen; password settings submenu (random or static); dim clock suppressed while server is running |
| SD apps | (discovered) | `/sd/apps/<id>/manifest.json` — MicroPython or native C binary |
| `testapp_native` | SDK Test (C) | Native C test app; exercises every SDK capability via a menu; `apps/testapp_native/` |
| `testapp_mp` | SDK Test (MP) | MicroPython test app; exercises every SDK capability exposed by `jppsdk`; `apps/testapp_mp/` |

## UNIQUE STYLES
- Native code should stay organized around ESP-IDF components and app/main entrypoints.
- Host-side checks live under `tests/`; scenario fixtures under `tests/fixtures/`.
- Keep docs concise and factual; prefer repo-backed commands over aspirational tooling.

## COMMANDS
```bash
idf.py set-target esp32c6
idf.py build
idf.py flash
docker compose run --rm build idf.py build
scripts/flash.sh
```

App artefacts after `idf.py build` — copy directory contents to `/sd/apps/<id>/`:
```
build/apps/meetapp/          meetapp.bin + manifest.json
build/apps/testapp_native/   testapp_native.bin + manifest.json
build/apps/testapp_mp/       main.mpy + manifest.json
```
`testapp_mp_bin` requires `mpy-cross` 1.28.0 on PATH: `pip install mpy-cross==1.28.0`.
Native `.bin` files are ELF32 shared objects; the `.bin` extension is what the firmware expects.

## JPPD-SMP WIRE FORMAT

J++Device Serial Management Protocol v1 — binary protocol over UART0 (115200 8N1), coexisting with ESP_LOG text output on the same physical channel. A TX mutex (registered via `esp_log_set_vprintf`) prevents log bytes from interleaving with binary frame bytes.

**Frame envelope** (both directions):
```
[SOF: 4 B]  0x01 0x4A 0x50 0x50  ("\x01JPP")
[LEN: 2 B LE]  byte count of PAYLOAD only
[PAYLOAD: LEN B]
[CRC: 2 B LE]  CRC-16/CCITT-FALSE (poly=0x1021, init=0xFFFF) over LEN(2)+PAYLOAD(LEN)
```

**Command payload** (host→device): `[SEQ:1][CMD:1][FLAGS:1][BODY…]`
**Response payload** (device→host): `[SEQ:1][STATUS:1][BODY…]`

`FLAGS` is reserved (must be 0x00). `SEQ` is echoed in the response for request/response matching. File operations are restricted to paths starting with `/sd`.

**Commands** (cmd byte → handler):
| CMD  | Name               | Body (host→device)                                         | OK body (device→host)                                        |
|------|--------------------|------------------------------------------------------------|--------------------------------------------------------------|
| 0x00 | SESSION_START      | `[proto_ver:1]`                                            | `[proto_ver:1]`                                              |
| 0x01 | SESSION_END        | —                                                          | —                                                            |
| 0x02 | GET_INFO           | —                                                          | `[fw_version: NUL-terminated string]`                        |
| 0x03 | GET_LRV_DATA       | —                                                          | `[cert: NUL-term][cert_sig: 64B][device_pubkey: 32B][challenge: NUL-term][resp_sig: 64B]` |
| 0x10 | FS_LIST_DIR        | `[path: NUL-terminated]`                                   | `[count:2 LE]` then N×`[flags:1][name: NUL-terminated]`     |
| 0x11 | FS_MKDIR           | `[path: NUL-terminated]`                                   | —                                                            |
| 0x12 | FS_REMOVE          | `[path: NUL-terminated]`                                   | —                                                            |
| 0x13 | FS_RENAME          | `[src: NUL-terminated][dst: NUL-terminated]`               | —                                                            |
| 0x14 | FS_UPLOAD_BEGIN    | `[file_size:4 LE][path: NUL-terminated]`                   | `[xfer_id:1]`                                                |
| 0x15 | FS_UPLOAD_CHUNK    | `[xfer_id:1][chunk_idx:2 LE][data…]`                       | `[chunk_idx:2 LE]`                                           |
| 0x16 | FS_UPLOAD_END      | `[xfer_id:1][crc32:4 LE]`                                  | —                                                            |
| 0x17 | FS_DOWNLOAD_BEGIN  | `[path: NUL-terminated]`                                   | `[xfer_id:1][file_size:4 LE][chunk_count:2 LE][crc32:4 LE]` |
| 0x18 | FS_DOWNLOAD_CHUNK  | `[xfer_id:1][chunk_idx:2 LE]`                              | `[xfer_id:1][chunk_idx:2 LE][data…]`                         |
| 0x19 | FS_DOWNLOAD_END    | `[xfer_id:1]`                                              | —                                                            |

`GET_LRV_DATA` requires LRV data to be unlocked (`jpp_lrv_is_unlocked()`); returns `ERR_DENIED` otherwise. Challenge string format: `{username}|{YYYY-MM-DDTHH:MM:SS}` using the device RTC at time of request. `resp_sig` is the Ed25519 signature over the challenge bytes (64 B raw). Response buffer is a static 512-byte array (`s_lrv_resp_buf`) in `jpp_serial_mgr.c`.
`FS_LIST_DIR` flags byte: bit 0 = 1 if directory, 0 if file; bits 1–7 reserved.
`FS_UPLOAD_END` / `FS_DOWNLOAD_BEGIN` CRC32 field: CRC-32/ISO-HDLC (`~esp_rom_crc32_le(~0u, data, len)`), matching Python's `zlib.crc32`.
Chunk size constant: `SMP_CHUNK_SIZE` = 1024 B. Max one active transfer at a time; `xfer_id` is always 0x00 in v1.

**Status codes**: 0x00 OK · 0x01 ERR_DENIED · 0x02 ERR_NOT_FOUND · 0x03 ERR_IO · 0x04 ERR_EXISTS · 0x05 ERR_INVALID · 0x06 ERR_BUSY · 0x07 ERR_NO_SESSION · 0x08 ERR_TRANSFER · 0x09 ERR_OVERFLOW · 0x0A ERR_APP_RUNNING

**Session rules**: SESSION_START triggers an OLED consent dialog; host blocks until user responds (Allow/Deny). An app must not be running (`s_active_sdk_context == NULL`). Session times out after `SMP_SESSION_TIMEOUT_MS` (30 s) of inactivity. Deep sleep is suppressed while the dialog or an active session is displayed. SD app launch is blocked while a session is open.

## NOTES
- The hardware profile remains ESP32-C6 Super Mini class with OLED, RTC, SD, keypad, and passive piezo buzzer support.
- DS1307 RTC is I²C-attached (0x68); CH-bit quirk handled in `jpp_rtc_hw_read()`.
- Buzzer: GPIO3 LP pad, LEDC PWM. Drive strength is varied at runtime for volume control (CAP_0–3 = 25–100%); CAP_3 is the boot default and the 100% level. Do not hard-code `GPIO_DRIVE_CAP_3` after init — use `jpp_buzzer_set_volume()` instead.
- Screen standby and sleep durations are configurable in the Settings > Sleep timers section.
- The current in-repo simulation board is `board-esp32-c6-devkitc-1`; Wokwi stays a reference only.
- Preserve error codes and troubleshooting markers in lower-level docs and runtime logs.
- WiFi/heap coexistence: the C6 has a single unified SRAM (no PSRAM). WiFi management/data frames and lwIP pbufs are allocated from the general heap, and TX management frames have no static-buffer config knob. Under heavy networking (a WebDAV transfer) low free heap makes those allocations fail (`wifi:m f ...`), silently wedging the radio until the load stops. Mitigations in place: `esp_wifi_set_ps(WIFI_PS_NONE)` (jpp_wifi_init.c) and **BLE controller suspend while any HTTP server runs** — the main-loop server-state poll (`app_main.c`) calls `jpp_ble_native_suspend()`/`_resume()` when WebDAV **or** the LRV server starts/stops (gated on `fileserver` state OR `jpp_lrv_server_is_running()`), freeing the NimBLE controller's heap (safe because both servers and SD apps are mutually exclusive, so no app uses BLE then; WebDAV and LRV are mutually exclusive with each other too). Global diagnostics: `jpp_heap_monitor` (started first thing in `app_main`) logs any failed allocation (`ALLOC FAILED ...`, tag `heap_mon`) and warns/errors on sustained low free heap — use this first when chasing a "network died" or OOM report. `jpp_fileserver_core` emits `heap @webdav-start`/`webdav-stop` event markers via `jpp_heap_monitor_log()`. The app memory pool (`jpp_app_pool`, `JPP_APP_POOL_BYTES` = 64 KB static BSS) holds native app code or the MicroPython GC heap (one at a time — apps are mutually exclusive) and cannot shrink below ~MeetApp's ~50 KB footprint; it was unified from the former separate 64 KB exec + 32 KB GC pools, returning ~32 KB to the general heap (the same heap WiFi/lwIP draw frame buffers from).
