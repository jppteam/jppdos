# SDK reference

Complete documentation for every App SDK call, split by category. This page is
the index; each entry below links straight to the function.

For each function the reference gives:

- the full **C** signature from `jpp_sdk_bridge.h`,
- the **MicroPython** binding from the `jppsdk` module, where one exists,
- the capability required — if none is listed, the call is **ungated**,
- the return value in both languages, including any output parameters,
- notes covering what the signature does not tell you.

Signatures are shown as **C** / **MicroPython** tabs, switched per signature.

!!! info "Errors in MicroPython."
    Unless a function's entry says otherwise, a `jppsdk` binding raises
    `jppsdk.SdkError` on any non-OK status, and `jppsdk.SdkPermissionError`
    (a subclass) when the capability was not granted. Catch the permission
    error separately and degrade — a denied capability is a normal outcome,
    not a crash.

!!! info "The two SDKs expose the same surface."
    Every SDK call has both a C and a MicroPython form, with two deliberate
    exceptions marked **(C only)**: [code modules](sdk/background.md#code-modules-native-apps-only)
    (`module_load`/`module_run`/`module_unload`), which load a second native ELF
    binary and have no meaning for a MicroPython app — use `import` instead —
    and [`push_key`](sdk/app-control.md#push_key), a firmware-internal input
    hook rather than an app-facing call.

---

## Index

### Core

| Page | Functions |
|------|-----------|
| [Types and constants](sdk/types.md) | [`jpp_sdk_status_t`](sdk/types.md#c--jpp_sdk_status_t) · [`jpp_sdk_key_event_t`](sdk/types.md#c--jpp_sdk_key_event_t) · [Python constants](sdk/types.md#python--jppsdk-constants) · [`jpp_broker_result_t`](sdk/types.md#c--jpp_broker_result_t) |
| [App control and input](sdk/app-control.md) | [`set_frame`](sdk/app-control.md#set_frame) · [`request_close`](sdk/app-control.md#request_close) · [`log`](sdk/app-control.md#log) · [`request_cap`](sdk/app-control.md#request_cap) · [`poll_key`](sdk/app-control.md#poll_key) · [`wait_key`](sdk/app-control.md#wait_key) · [`push_key`](sdk/app-control.md#push_key) · [`claim_center`](sdk/app-control.md#claim_center) |
| [Canvas and UI helpers](sdk/display.md) | [`canvas_write`](sdk/display.md#canvas_write) · [`canvas_draw_pixel`](sdk/display.md#canvas_draw_pixel) · [`canvas_clear`](sdk/display.md#canvas_clear) · [`canvas_fullscreen`](sdk/display.md#canvas_fullscreen) · [`dialog`](sdk/display.md#dialog) · [`list`](sdk/display.md#list) · [`input`](sdk/display.md#input) · [`confirm`](sdk/display.md#confirm) · [`file_pick`](sdk/display.md#file_pick) · [`wrap_text`](sdk/display.md#wrap_text) |
| [Buzzer, LED, device status](sdk/hardware.md) | [`buzzer_play`](sdk/hardware.md#buzzer_play) · [`buzzer_tone`](sdk/hardware.md#buzzer_tone) · [`buzzer_play_sequence`](sdk/hardware.md#buzzer_play_sequence) · [`buzzer_play_sequence_async`](sdk/hardware.md#buzzer_play_sequence_async) · [`buzzer_stop`](sdk/hardware.md#buzzer_stop) · [`led_set_color`](sdk/hardware.md#led_set_color) · [`led_off`](sdk/hardware.md#led_off) · [`wakelock_acquire`](sdk/hardware.md#wakelock_acquire) · [`wakelock_release`](sdk/hardware.md#wakelock_release) · [`device_status`](sdk/hardware.md#device_status) · [`get_time`](sdk/hardware.md#get_time) · [`is_dummy_mode`](sdk/hardware.md#is_dummy_mode) |

### Data

| Page | Functions |
|------|-----------|
| [Storage and IPC](sdk/storage.md) | [`file_read`](sdk/storage.md#file_read) · [`file_write`](sdk/storage.md#file_write) · [`file_list`](sdk/storage.md#file_list) · [shared I/O](sdk/storage.md#shared-file-io) · [`kv_get`](sdk/storage.md#kv_get) · [`kv_set`](sdk/storage.md#kv_set) · [`kv_delete`](sdk/storage.md#kv_delete) · [`ipc_send`](sdk/storage.md#ipc_send) · [`ipc_recv`](sdk/storage.md#ipc_recv) |
| [Full file access](sdk/storage.md#full-file-access) ⚠ `files.full` | [`file_open`](sdk/storage.md#file_open) · [`handle_read`](sdk/storage.md#handle_read) · [`handle_write`](sdk/storage.md#handle_write) · [`handle_list`](sdk/storage.md#handle_list) · [`handle_close`](sdk/storage.md#handle_close) |

### Connectivity

| Page | Functions |
|------|-----------|
| [HTTP](sdk/network.md#http) ⚠ `http.request` | [`http_request`](sdk/network.md#http_request) |
| [HTTPS](sdk/network.md#https) ⚠ `https.request` | [`https_request`](sdk/network.md#https_request) · [per-origin consent](sdk/network.md#per-origin-consent) |
| [TCP server](sdk/network.md#tcp-server) ⚠ `network.bind` | [`net_bind`](sdk/network.md#net_bind) · [`net_accept`](sdk/network.md#net_accept) · [`net_recv`](sdk/network.md#net_recv) · [`net_send`](sdk/network.md#net_send) · [`net_close`](sdk/network.md#net_close) |
| [TCP client](sdk/network.md#tcp-client) ⚠ `network.connect` | [`net_connect`](sdk/network.md#net_connect) · shares `net_recv`/`net_send`/`net_close` |
| [BLE scan](sdk/wireless.md#ble-scan) ⚠ `ble.scan` | [`ble_scan`](sdk/wireless.md#ble_scan) |
| [BLE advertise](sdk/wireless.md#ble-advertise) ⚠ `ble.advertise` | [`ble_advertise_start`](sdk/wireless.md#ble_advertise_start) · [`ble_advertise_stop`](sdk/wireless.md#ble_advertise_stop) · [`ble_set_connectable`](sdk/wireless.md#ble_set_connectable) |
| [BLE GATT client](sdk/wireless.md#ble-gatt-client) ⚠ `ble.connect` | [`ble_connect`](sdk/wireless.md#ble_connect) · [`ble_read_char`](sdk/wireless.md#ble_read_char) · [`ble_write_char`](sdk/wireless.md#ble_write_char) · [`ble_disconnect`](sdk/wireless.md#ble_disconnect) |
| [BLE GATT server](sdk/wireless.md#ble-gatt-server) ⚠ `ble.host` | [`ble_service_register`](sdk/wireless.md#ble_service_register) · [`ble_service_unregister`](sdk/wireless.md#ble_service_unregister) · [`ble_host_set_value`](sdk/wireless.md#ble_host_set_value) · [`ble_host_wait_write`](sdk/wireless.md#ble_host_wait_write) · [`ble_host_clear`](sdk/wireless.md#ble_host_clear) |
| [ESP-NOW](sdk/wireless.md#esp-now) ⚠ `esp_now` | [`espnow_send`](sdk/wireless.md#espnow_send) · [`espnow_recv`](sdk/wireless.md#espnow_recv) |

### Compute and lifecycle

| Page | Functions |
|------|-----------|
| [Crypto primitives](sdk/crypto.md) | [`sha256`](sdk/crypto.md#jpp_crypto_sha256) · [`sha1`](sdk/crypto.md#jpp_crypto_sha1) · [`aes256_ige_encrypt`/`_decrypt`](sdk/crypto.md#jpp_crypto_aes256_ige_encrypt) · [`modexp`](sdk/crypto.md#jpp_crypto_modexp) · [`rsa_encrypt`](sdk/crypto.md#jpp_crypto_rsa_encrypt) · [`dh_compute`](sdk/crypto.md#jpp_crypto_dh_compute) |
| [Background tasks](sdk/background.md#background-registration) ⚠ `background.register` | [`background_register`](sdk/background.md#background_register) |
| [Code modules](sdk/background.md#code-modules-native-apps-only) (native only) | [`module_load`](sdk/background.md#jpp_sdk_module_load) · [`module_run`](sdk/background.md#jpp_sdk_module_run) · [`module_unload`](sdk/background.md#jpp_sdk_module_unload) |
| [Resource limits](sdk/limits.md) | every hard cap in one table |

⚠ = capability required. See [Capabilities](manifest.md#capabilities) for the
tier rules and what each one unlocks.

---

## Which SDK level do I need?

The firmware exports a single API **level**, and an app declares the lowest one
it can run on via [`sdk_min`](manifest.md#sdk_min). Everything in this reference
is available at level 2 (firmware v1.1). The calls that are *not* available at
level 1 are marked in place with a "Requires SDK level 2" note.

The [SDK changelog](sdk-changelog.md) lists every level, what it added, and how
to pick a value for `sdk_min`.
