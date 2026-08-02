# SDK Capability Expansion Plan

Covers 12 new capabilities across the ungated and tier-1 tiers.
No GPIO, I2C, SPI, UART, or CAN additions — the hardware profile is fixed
and no expansion pins are broken out.

---

## Files touched, in dependency order

```
jpp_resource_budget.h           lock-limit bump, new UDP socket constant
jpp_manifest_core.c             register new tier-1 capability strings
jpp_sdk_bridge.h                all new callback typedefs, service fields, decls
jpp_sdk_bridge.c                all ungated implementations + tier-1 bridge fns
jpp_native_services.c/.h        concrete callback implementations
jpp_mp_sdk_module.c             Python bindings for every new function
main/CMakeLists.txt             add mdns, driver (temperature sensor)
jpp_wifi_init.c/.h              add wifi_get_ip()
jpp_app_dispatch.c              teardown hooks for mdns + http.server
docs/sdk-reference.md           full API docs for new surface
docs/manifest.md                new tier-1 capability strings
AGENTS.md                       CODE MAP and CONVENTIONS update
```

---

## Step 0 — Foundation: resource budget and capability registry

Must land before everything else.

### `jpp_resource_budget.h`

```c
#define JPP_RESOURCE_BROKER_LOCK_LIMIT  12u   /* was 8; slots: storage, device,
    vm_queue, ble_radio, ble_client, ble_host, http, network,
    mdns, http_server, (2 reserved) */
#define JPP_RESOURCE_SDK_UDP_SOCKET_LIMIT 1u
```

### `jpp_manifest_core.c` — `jpp_manifest_v2_is_allowed_capability()`

Add to the tier-1 block:

```c
jpp_str_eq(capability, "net.connect") ||
jpp_str_eq(capability, "net.udp")     ||
jpp_str_eq(capability, "net.mdns")    ||
jpp_str_eq(capability, "http.server")
```

---

## Step 1 — `storage.unlink` and `storage.move` (ungated)

`sd_file_delete_cb` / `services.delete_file` already exist and are wired in
`jpp_native_services_init()` but are only called internally by `jpp_sdk_ipc_recv`.

### `jpp_sdk_bridge.h`

New callback typedef for move (delete already has `jpp_sdk_file_deleter_t`):

```c
typedef jpp_broker_status_t (*jpp_sdk_file_move_fn_t)(
    void *context,
    const char *src_path,
    const char *dst_path,
    jpp_broker_result_t *result
);
```

New field in `jpp_sdk_native_services_t`:

```c
jpp_sdk_file_move_fn_t  move_file;
void                   *move_file_context;
```

New public functions:

```c
/* Ungated — delete a file in the app's scoped (/sd/apps/<id>/) storage. */
jpp_sdk_status_t jpp_sdk_file_delete(jpp_sdk_context_t *ctx,
                                     const char *relative_path,
                                     jpp_broker_result_t *result);

/* Ungated — delete a file in the app's shared (/sd/shared/<id>/) storage. */
jpp_sdk_status_t jpp_sdk_shared_delete(jpp_sdk_context_t *ctx,
                                       const char *relative_path,
                                       jpp_broker_result_t *result);

/* Requires: files.full — delete the file referred to by an open handle. */
jpp_sdk_status_t jpp_sdk_handle_delete(jpp_sdk_context_t *ctx,
                                       jpp_sdk_handle_t handle,
                                       jpp_broker_result_t *result);

/* Ungated — move/rename within the app's scoped storage. Both paths are
   relative to /sd/apps/<id>/. Cross-root moves (scoped ↔ shared) are not
   allowed; use read + write + delete for that. */
jpp_sdk_status_t jpp_sdk_file_move(jpp_sdk_context_t *ctx,
                                   const char *src_relative,
                                   const char *dst_relative,
                                   jpp_broker_result_t *result);

/* Ungated — move/rename within the app's shared storage. */
jpp_sdk_status_t jpp_sdk_shared_move(jpp_sdk_context_t *ctx,
                                     const char *src_relative,
                                     const char *dst_relative,
                                     jpp_broker_result_t *result);
```

### `jpp_sdk_bridge.c`

Add private `jpp_sdk_do_delete()` helper (mirrors `jpp_sdk_do_read`): builds an
absolute scoped or shared path, calls `services.delete_file` under the `"storage"`
exclusive broker lock. `jpp_sdk_file_delete` and `jpp_sdk_shared_delete` call it
with `/sd/apps` and `/sd/shared` roots respectively. `jpp_sdk_handle_delete`
resolves the handle path, checks `files.full` via `jpp_sdk_ensure_cap`, then
calls the same helper.

Add private `jpp_sdk_do_move()` helper: validates both relative paths, builds
both absolute paths from the same root, calls `services.move_file` under the
`"storage"` lock.

### `jpp_native_services.c`

New callback:

```c
static jpp_broker_status_t sd_file_move_cb(void *context,
                                            const char *src, const char *dst,
                                            jpp_broker_result_t *result)
{
    (void)context;
    jpp_make_parent_dirs(dst);
    if (rename(src, dst) != 0) {
        jpp_broker_error_result(result, "MOVE_FAILED");
        return JPP_BROKER_STATUS_OK;
    }
    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}
```

Wire in `jpp_native_services_init()`:

```c
s_native_services.move_file         = sd_file_move_cb;
s_native_services.move_file_context = NULL;
```

### `jpp_mp_sdk_module.c`

```
jppsdk.storage_delete(path: str) -> None
jppsdk.shared_delete(path: str) -> None
jppsdk.storage_move(src: str, dst: str) -> None
jppsdk.shared_move(src: str, dst: str) -> None
```

---

## Step 2 — `canvas.text`, `canvas.line`, `canvas.rect` (ungated)

All implemented in `jpp_sdk_bridge.c` only. No new callbacks or native_services
fields needed.

### `jpp_sdk_bridge.h`

```c
/* Draw a NUL-terminated string starting at pixel (x, y). Characters are 5×8 px
   with a 1-px column gap; clipped at the canvas right edge. */
jpp_sdk_status_t jpp_sdk_canvas_text(jpp_sdk_context_t *ctx,
                                     uint8_t x, uint8_t y,
                                     const char *str);

/* Bresenham line from (x0,y0) to (x1,y1). on=true draws, on=false erases. */
jpp_sdk_status_t jpp_sdk_canvas_line(jpp_sdk_context_t *ctx,
                                     uint8_t x0, uint8_t y0,
                                     uint8_t x1, uint8_t y1,
                                     bool on);

/* Axis-aligned rectangle. filled=true fills the interior; false = outline. */
jpp_sdk_status_t jpp_sdk_canvas_rect(jpp_sdk_context_t *ctx,
                                     uint8_t x, uint8_t y,
                                     uint8_t w, uint8_t h,
                                     bool filled);
```

### `jpp_sdk_bridge.c`

Embed `static const uint8_t JPP_SDK_FONT_5X8[95][5]` in flash `.rodata`
(printable ASCII 0x20–0x7E, 475 bytes). Each entry is 5 column bytes, bit 0 =
top row, matching the SSD1306 page orientation.

- `jpp_sdk_canvas_text`: iterate characters, look up 5-column glyph, call
  `jpp_sdk_canvas_draw_pixel` for each set bit; advance x by 6 (5 + 1-px gap).
  Clip when `x + 5 > 128`. Respect fullscreen vs windowed y bounds (47 vs 63).
- `jpp_sdk_canvas_line`: integer Bresenham, calls `jpp_sdk_canvas_draw_pixel`.
- `jpp_sdk_canvas_rect`: outline = 4 line calls; filled = horizontal span loop.

### `jpp_mp_sdk_module.c`

```
jppsdk.canvas_text(x: int, y: int, s: str) -> None
jppsdk.canvas_line(x0: int, y0: int, x1: int, y1: int, on: bool = True) -> None
jppsdk.canvas_rect(x: int, y: int, w: int, h: int, filled: bool = False) -> None
```

---

## Step 3 — `hardware.rng` (ungated)

`esp_random()` / `esp_fill_random()` have no resource conflict. Called directly
in the bridge without a callback.

### `jpp_sdk_bridge.h`

```c
/* True random bytes from the hardware RNG. Requires at least one RF subsystem
   (WiFi/BLE/802.15.4) active for full hardware entropy; degrades to
   ADC-seeded PRNG when all radios are off. */
jpp_sdk_status_t jpp_sdk_random_bytes(jpp_sdk_context_t *ctx,
                                      uint8_t *buf, size_t len);
jpp_sdk_status_t jpp_sdk_random_u32(jpp_sdk_context_t *ctx, uint32_t *out);
```

### `jpp_sdk_bridge.c`

Add `#include "esp_random.h"`. Both functions check `jpp_sdk_ensure_bound`
then call `esp_fill_random` / `esp_random` directly. No broker lock.

### `jpp_mp_sdk_module.c`

```
jppsdk.random_bytes(n: int) -> bytes
jppsdk.random_u32() -> int
```

---

## Step 4 — `device.status` returns username (ungated)

The function signature of `jpp_sdk_device_status` is unchanged; the result dict
gains a `"username"` field.

### `jpp_native_services.c` — extend `device_status_cb`

```c
static char s_dev_username[JPP_SETTINGS_USERNAME_MAX + 1u];
size_t ulen = sizeof(s_dev_username);
nvs_handle_t nvs;
if (nvs_open("jpp_user", NVS_READONLY, &nvs) == ESP_OK) {
    if (nvs_get_str(nvs, "username", s_dev_username, &ulen) != ESP_OK)
        s_dev_username[0] = '\0';
    nvs_close(nvs);
} else {
    s_dev_username[0] = '\0';
}
jpp_broker_result_put(result, "username", s_dev_username);
```

The static buffer is safe because the callback runs under the `"device"`
exclusive broker lock. Total result fields: `battery_pct` + `charging` +
`username` = 3, within `FIELD_LIMIT = 8`.

MicroPython apps get `jppsdk.device_status()["username"]` with no code change.

---

## Step 5 — `sensor.temperature` (ungated)

### `jpp_sdk_bridge.h`

```c
typedef jpp_broker_status_t (*jpp_sdk_temp_reader_fn_t)(
    void *context,
    float *out_celsius,
    jpp_broker_result_t *result
);
/* in jpp_sdk_native_services_t: */
jpp_sdk_temp_reader_fn_t  temp_reader;
void                     *temp_reader_context;

/* Ungated. Reads the ESP32-C6 internal die temperature sensor (±2 °C).
   Die temperature is ~20–30 °C above ambient under full CPU load.
   Returns INVALID_STATE when no sensor driver is available. */
jpp_sdk_status_t jpp_sdk_temperature_read(jpp_sdk_context_t *ctx,
                                          float *out_celsius);
```

### `main/CMakeLists.txt`

Add `driver` to `REQUIRES`.

### `jpp_native_services.c`

```c
#include "driver/temperature_sensor.h"

static temperature_sensor_handle_t s_temp_sensor = NULL;

/* In jpp_native_services_init(): */
temperature_sensor_config_t ts_cfg =
    TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);   /* range 10–80 °C */
if (temperature_sensor_install(&ts_cfg, &s_temp_sensor) == ESP_OK)
    temperature_sensor_enable(s_temp_sensor);

s_native_services.temp_reader         = temp_reader_cb;
s_native_services.temp_reader_context = NULL;

static jpp_broker_status_t temp_reader_cb(void *context,
                                           float *out_celsius,
                                           jpp_broker_result_t *result)
{
    (void)context;
    if (s_temp_sensor == NULL) {
        jpp_broker_error_result(result, "SENSOR_UNAVAILABLE");
        return JPP_BROKER_STATUS_OK;
    }
    if (temperature_sensor_get_celsius(s_temp_sensor, out_celsius) != ESP_OK) {
        jpp_broker_error_result(result, "READ_FAILED");
        return JPP_BROKER_STATUS_OK;
    }
    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}
```

No broker lock needed — only one app runs at a time and the sensor is
stateless/non-destructive.

### `jpp_mp_sdk_module.c`

```
jppsdk.temperature_read() -> float   # degrees Celsius
```

---

## Step 6 — `net.dns` (ungated)

### `jpp_sdk_bridge.h`

```c
typedef jpp_broker_status_t (*jpp_sdk_dns_lookup_fn_t)(
    void *context,
    const char *hostname,
    char *out_ip,
    size_t out_ip_len,
    jpp_broker_result_t *result
);
/* in jpp_sdk_native_services_t: */
jpp_sdk_dns_lookup_fn_t  dns_lookup;
void                    *dns_lookup_context;

/* Ungated. Resolves hostname to a dotted-decimal IPv4 string.
   Blocks for up to ~5 s on slow WiFi. out_ip must be at least 16 bytes.
   Returns BROKER_ERROR ("DNS_FAILED") when WiFi is not connected or
   the name cannot be resolved. */
jpp_sdk_status_t jpp_sdk_dns_lookup(jpp_sdk_context_t *ctx,
                                    const char *hostname,
                                    char *out_ip,
                                    size_t out_ip_len);
```

### `jpp_native_services.c`

```c
static jpp_broker_status_t dns_lookup_cb(void *context, const char *hostname,
                                          char *out_ip, size_t out_ip_len,
                                          jpp_broker_result_t *result)
{
    (void)context;
    struct addrinfo hints = { .ai_family = AF_INET,
                              .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(hostname, NULL, &hints, &res) != 0 || res == NULL) {
        jpp_broker_error_result(result, "DNS_FAILED");
        return JPP_BROKER_STATUS_OK;
    }
    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &addr->sin_addr, out_ip, (socklen_t)out_ip_len);
    freeaddrinfo(res);
    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}
```

`getaddrinfo` is thread-safe in lwIP. No exclusive broker lock needed.

### `jpp_mp_sdk_module.c`

```
jppsdk.dns_lookup(hostname: str) -> str   # "NNN.NNN.NNN.NNN"
```

Raises `SdkError("DNS_FAILED")` when resolution fails.

---

## Step 7 — `crypto.hmac` (ungated)

libsodium is already a component dependency (used by `jpp_crypto_core` and `jpp_lrv.c`).
No callbacks or native_services fields required.

### `jpp_sdk_bridge.h`

```c
#define JPP_SDK_HMAC_SHA256_LEN 32u

/* Ungated. Computes HMAC-SHA256(key, data) into out_mac[32] using libsodium.
   Fully deterministic — does not consume hardware entropy.
   Keys longer than 64 bytes are hashed by the HMAC construction internally. */
jpp_sdk_status_t jpp_sdk_hmac_sha256(jpp_sdk_context_t *ctx,
                                     const uint8_t *key,  size_t key_len,
                                     const uint8_t *data, size_t data_len,
                                     uint8_t out_mac[JPP_SDK_HMAC_SHA256_LEN]);
```

### `jpp_sdk_bridge.c`

```c
#include "sodium/crypto_auth_hmacsha256.h"

jpp_sdk_status_t jpp_sdk_hmac_sha256(jpp_sdk_context_t *ctx,
                                     const uint8_t *key,  size_t key_len,
                                     const uint8_t *data, size_t data_len,
                                     uint8_t out_mac[JPP_SDK_HMAC_SHA256_LEN])
{
    jpp_sdk_status_t st = jpp_sdk_ensure_bound(ctx);
    if (st != JPP_SDK_STATUS_OK) return st;
    if (key == NULL || data == NULL || out_mac == NULL)
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    crypto_auth_hmacsha256_state state;
    crypto_auth_hmacsha256_init(&state, key, key_len);
    crypto_auth_hmacsha256_update(&state, data, data_len);
    crypto_auth_hmacsha256_final(&state, out_mac);
    return JPP_SDK_STATUS_OK;
}
```

### `jpp_mp_sdk_module.c`

```
jppsdk.hmac_sha256(key: bytes, data: bytes) -> bytes   # always 32 bytes
```

---

## Step 8 — `system.info` (ungated)

Exposes runtime metrics that are already computed by other firmware modules but
not reachable from apps.

### New public functions in `jpp_wifi_init.c/.h`

`wifi_get_connected_ssid` already exists but the IP address is only logged at
connection time (`WIFI_GOT_IP`). Add a cached copy:

```c
/* jpp_wifi_init.h */
bool wifi_get_ip(char *out, size_t out_len);   /* "NNN.NNN.NNN.NNN" or "" */
```

```c
/* jpp_wifi_init.c — in the IP_EVENT_STA_GOT_IP handler: */
static char s_connected_ip[16] = {0};
inet_ntoa_r(ev->ip_info.ip, s_connected_ip, sizeof(s_connected_ip));

/* Clear on disconnect (WIFI_EVENT_STA_DISCONNECTED): */
s_connected_ip[0] = '\0';

bool wifi_get_ip(char *out, size_t out_len) {
    if (!s_has_ip || s_connected_ip[0] == '\0') {
        if (out && out_len) out[0] = '\0';
        return false;
    }
    strncpy(out, s_connected_ip, out_len);
    out[out_len - 1u] = '\0';
    return true;
}
```

### `jpp_sdk_bridge.h`

```c
typedef jpp_broker_status_t (*jpp_sdk_system_info_fn_t)(
    void *context,
    jpp_broker_result_t *result
);
/* in jpp_sdk_native_services_t: */
jpp_sdk_system_info_fn_t  system_info;
void                     *system_info_context;

/* Ungated. Returns a snapshot of current system metrics.
   Result fields (all values are strings):
     "free_heap"     — current free internal heap bytes
     "min_free_heap" — historical minimum free heap since boot (low-water mark)
     "uptime_ms"     — milliseconds since boot (wraps after ~49 days)
     "wifi_ssid"     — connected SSID, or "" if not connected
     "wifi_ip"       — IPv4 address as "NNN.NNN.NNN.NNN", or ""
     "wifi_rssi"     — signal strength in dBm, or "0" if not connected
     "sd_total_kb"   — total SD card capacity in KB (0 if SD not mounted)
     "sd_free_kb"    — free SD card space in KB */
jpp_sdk_status_t jpp_sdk_system_info(jpp_sdk_context_t *ctx,
                                     jpp_broker_result_t *result);
```

8 fields fit comfortably within `FIELD_LIMIT = 8` (the `device` lock's result
table). The call runs under the `"device"` exclusive lock so the static number
buffers in the callback are safe.

### `jpp_native_services.c`

```c
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "jpp_wifi_init.h"

static jpp_broker_status_t system_info_cb(void *context,
                                           jpp_broker_result_t *result)
{
    (void)context;

    static char s_free_heap[12], s_min_heap[12], s_uptime[16];
    static char s_ssid[33], s_ip[16], s_rssi[8];
    static char s_sd_total[16], s_sd_free[16];

    snprintf(s_free_heap, sizeof(s_free_heap), "%lu",
             (unsigned long)esp_get_free_heap_size());
    snprintf(s_min_heap,  sizeof(s_min_heap),  "%lu",
             (unsigned long)esp_get_minimum_free_heap_size());
    snprintf(s_uptime,    sizeof(s_uptime),    "%llu",
             (unsigned long long)(esp_timer_get_time() / 1000ULL));

    bool connected = wifi_is_connected();
    if (connected) {
        wifi_get_connected_ssid(s_ssid, sizeof(s_ssid));
        wifi_get_ip(s_ip, sizeof(s_ip));
        wifi_ap_record_t ap;
        int8_t rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
        snprintf(s_rssi, sizeof(s_rssi), "%d", (int)rssi);
    } else {
        s_ssid[0] = '\0'; s_ip[0] = '\0';
        snprintf(s_rssi, sizeof(s_rssi), "0");
    }

    uint64_t sd_total = 0u, sd_free_bytes = 0u;
    if (esp_vfs_fat_info("/sd", &sd_total, &sd_free_bytes) == ESP_OK) {
        snprintf(s_sd_total, sizeof(s_sd_total), "%llu", sd_total / 1024ULL);
        snprintf(s_sd_free,  sizeof(s_sd_free),  "%llu", sd_free_bytes / 1024ULL);
    } else {
        snprintf(s_sd_total, sizeof(s_sd_total), "0");
        snprintf(s_sd_free,  sizeof(s_sd_free),  "0");
    }

    jpp_broker_ok_result(result);
    jpp_broker_result_put(result, "free_heap",     s_free_heap);
    jpp_broker_result_put(result, "min_free_heap", s_min_heap);
    jpp_broker_result_put(result, "uptime_ms",     s_uptime);
    jpp_broker_result_put(result, "wifi_ssid",     s_ssid);
    jpp_broker_result_put(result, "wifi_ip",       s_ip);
    jpp_broker_result_put(result, "wifi_rssi",     s_rssi);
    jpp_broker_result_put(result, "sd_total_kb",   s_sd_total);
    jpp_broker_result_put(result, "sd_free_kb",    s_sd_free);
    return JPP_BROKER_STATUS_OK;
}
```

Wire in `jpp_native_services_init()`:

```c
s_native_services.system_info         = system_info_cb;
s_native_services.system_info_context = NULL;
```

Also add `#include "esp_vfs_fat.h"` and `#include "esp_wifi.h"` if not already
present.

### `jpp_mp_sdk_module.c`

```
jppsdk.system_info() -> dict
# keys: "free_heap", "min_free_heap", "uptime_ms",
#       "wifi_ssid", "wifi_ip", "wifi_rssi",
#       "sd_total_kb", "sd_free_kb"
# All values are strings; parse with int() / float() as needed.
```

---

## Step 9 — `net.connect` (tier-1, outbound TCP client)

### `jpp_sdk_bridge.h`

```c
typedef jpp_broker_status_t (*jpp_sdk_net_connect_fn_t)(
    void *context,
    const char *host,
    uint16_t port,
    uint32_t timeout_ms,
    int *out_sock,
    jpp_broker_result_t *result
);
/* in jpp_sdk_native_services_t: */
jpp_sdk_net_connect_fn_t net_connect;
/* net_context is shared with the existing bind/recv/send/close fields */

/* Requires: net.connect
   Open an outbound TCP connection to host:port. On success writes a socket
   descriptor to *out_sock. The descriptor is usable with jpp_sdk_net_send,
   jpp_sdk_net_recv, and jpp_sdk_net_close exactly like an accepted connection.
   Counts against JPP_RESOURCE_SDK_NET_SOCKET_LIMIT (shared with server
   connections). No exclusion with WebDAV or LRV — outbound TCP is independent
   of inbound servers. */
jpp_sdk_status_t jpp_sdk_net_connect(jpp_sdk_context_t *ctx,
                                     const char *host,
                                     uint16_t port,
                                     uint32_t timeout_ms,
                                     int *out_sock,
                                     jpp_broker_result_t *result);
```

### `jpp_native_services.c`

Implementation:
1. Find a free slot in `s_net_conns[]`; return `"SOCKET_LIMIT"` if full.
2. `getaddrinfo(host, portstr, &hints, &res)` with `AI_NUMERICSERV`.
3. `socket(AF_INET, SOCK_STREAM, 0)` + `fcntl(O_NONBLOCK)` + `connect()`.
4. `select()` up to `timeout_ms` for writability.
5. `getsockopt(SOL_SOCKET, SO_ERROR)` to verify the connection succeeded.
6. `fcntl(~O_NONBLOCK)` to restore blocking mode.
7. Store fd in `s_net_conns[slot]`; write to `*out_sock`.
8. On any failure: `close(fd)`, return error code.

`net_close_all_cb` already closes every entry in `s_net_conns[]` — no change
needed for teardown.

### `jpp_sdk_bridge.c`

`jpp_sdk_net_connect` calls `jpp_sdk_ensure_cap("net.connect")` then calls
`net_connect_cb` directly (no broker lock, matching the pattern of the existing
`net_bind` / `net_recv` / `net_send` calls).

### `jpp_mp_sdk_module.c`

```
jppsdk.net_connect(host: str, port: int, timeout_ms: int) -> int   # socket fd
```

---

## Step 10 — `net.udp` (tier-1, UDP sockets)

### `jpp_sdk_bridge.h`

```c
#define JPP_SDK_UDP_IP_LEN 16u   /* "NNN.NNN.NNN.NNN\0" */

typedef jpp_broker_status_t (*jpp_sdk_net_udp_bind_fn_t)(
    void *context, uint16_t port, jpp_broker_result_t *result);
typedef jpp_broker_status_t (*jpp_sdk_net_udp_sendto_fn_t)(
    void *context, const char *host, uint16_t port,
    const uint8_t *data, size_t len, jpp_broker_result_t *result);
typedef jpp_broker_status_t (*jpp_sdk_net_udp_recvfrom_fn_t)(
    void *context, uint8_t *buf, size_t buf_len, size_t *out_len,
    char *out_src_ip, uint16_t *out_src_port,
    uint32_t timeout_ms, jpp_broker_result_t *result);
typedef void (*jpp_sdk_net_udp_close_fn_t)(void *context);

/* in jpp_sdk_native_services_t: */
jpp_sdk_net_udp_bind_fn_t      net_udp_bind;
jpp_sdk_net_udp_sendto_fn_t    net_udp_sendto;
jpp_sdk_net_udp_recvfrom_fn_t  net_udp_recvfrom;
jpp_sdk_net_udp_close_fn_t     net_udp_close;
/* net_context shared with TCP callbacks */

/* Requires: net.udp
   Bind a UDP socket to port (0 = OS-assigned ephemeral). Optional — if the
   app only sends and never receives, skip and call sendto directly; sendto
   opens an unbound socket implicitly. One UDP socket per app session. */
jpp_sdk_status_t jpp_sdk_net_udp_bind(jpp_sdk_context_t *ctx,
                                      uint16_t port,
                                      jpp_broker_result_t *result);

/* Requires: net.udp — send a datagram to host:port. */
jpp_sdk_status_t jpp_sdk_net_udp_sendto(jpp_sdk_context_t *ctx,
                                        const char *host, uint16_t port,
                                        const uint8_t *data, size_t len,
                                        jpp_broker_result_t *result);

/* Requires: net.udp — wait up to timeout_ms for an incoming datagram.
   out_src_ip must be JPP_SDK_UDP_IP_LEN bytes. result->ok = false on timeout.
   result field "closed" is not set (UDP has no connection state). */
jpp_sdk_status_t jpp_sdk_net_udp_recvfrom(jpp_sdk_context_t *ctx,
                                           uint8_t *buf, size_t buf_len,
                                           size_t *out_len,
                                           char *out_src_ip,
                                           uint16_t *out_src_port,
                                           uint32_t timeout_ms,
                                           jpp_broker_result_t *result);

/* No cap required — teardown-safe close. No-op when no socket is open. */
void jpp_sdk_net_udp_close(jpp_sdk_context_t *ctx);
```

### `jpp_native_services.c`

Add `static int s_net_udp_sock = -1`.

- `net_udp_bind_cb`: if `s_net_udp_sock >= 0` return `"ALREADY_BOUND"`. Create
  `SOCK_DGRAM`, `setsockopt SO_REUSEADDR`, `bind`. Store in `s_net_udp_sock`.
  Result field `"port"` = actual bound port (useful when port was 0:
  `getsockname` to retrieve).
- `net_udp_sendto_cb`: if `s_net_udp_sock < 0`, create unbound `SOCK_DGRAM`
  and store in `s_net_udp_sock`. `getaddrinfo` + `sendto`.
- `net_udp_recvfrom_cb`: `select()` timeout + `recvfrom()`. Write source address
  via `inet_ntop`, source port via `ntohs`.
- `net_udp_close_cb`: `close(s_net_udp_sock); s_net_udp_sock = -1`.

Update `net_close_all_cb` to also call `net_udp_close_cb(NULL)`.

### `jpp_mp_sdk_module.c`

```
jppsdk.net_udp_bind(port: int) -> int       # actual port (useful if 0 passed)
jppsdk.net_udp_sendto(host: str, port: int, data: bytes) -> None
jppsdk.net_udp_recvfrom(buf_len: int, timeout_ms: int)
    -> tuple[bytes, str, int] | None        # (data, src_ip, src_port)
jppsdk.net_udp_close() -> None
```

---

## Step 11 — `net.mdns` (tier-1, mDNS service announce and browse)

### `main/CMakeLists.txt`

Add `mdns` to `REQUIRES`.

### `jpp_sdk_bridge.h`

```c
typedef jpp_broker_status_t (*jpp_sdk_mdns_start_fn_t)(
    void *context, const char *hostname, jpp_broker_result_t *result);
typedef jpp_broker_status_t (*jpp_sdk_mdns_announce_fn_t)(
    void *context, const char *service_type, const char *proto,
    const char *instance, uint16_t port, jpp_broker_result_t *result);
typedef jpp_broker_status_t (*jpp_sdk_mdns_query_fn_t)(
    void *context, const char *service_type, const char *proto,
    uint32_t timeout_ms, jpp_broker_result_t *result);
typedef void (*jpp_sdk_mdns_stop_fn_t)(void *context);

/* in jpp_sdk_native_services_t: */
jpp_sdk_mdns_start_fn_t    mdns_start;
jpp_sdk_mdns_announce_fn_t mdns_announce;
jpp_sdk_mdns_query_fn_t    mdns_query;
jpp_sdk_mdns_stop_fn_t     mdns_stop;
void                       *mdns_context;

/* Requires: net.mdns
   Initialize mDNS and set the device hostname for this session. Calling
   again before stop re-initializes with the new hostname.
   WARNING: hostname is device-global and visible on the local network
   for the duration of the app session. */
jpp_sdk_status_t jpp_sdk_mdns_start(jpp_sdk_context_t *ctx,
                                    const char *hostname,
                                    jpp_broker_result_t *result);

/* Requires: net.mdns — advertise a service record. proto is "tcp" or "udp". */
jpp_sdk_status_t jpp_sdk_mdns_announce(jpp_sdk_context_t *ctx,
                                       const char *service_type,
                                       const char *proto,
                                       const char *instance,
                                       uint16_t port,
                                       jpp_broker_result_t *result);

/* Requires: net.mdns — browse for peers offering service_type._proto.local.
   Blocks for timeout_ms. Result fields: "count" (N found, max 5) plus
   "entry_0".."entry_N-1" each formatted as "hostname:port". */
jpp_sdk_status_t jpp_sdk_mdns_query(jpp_sdk_context_t *ctx,
                                    const char *service_type,
                                    const char *proto,
                                    uint32_t timeout_ms,
                                    jpp_broker_result_t *result);

/* No cap required — stop mDNS. Called automatically at app teardown. */
void jpp_sdk_mdns_stop(jpp_sdk_context_t *ctx);
```

### `jpp_native_services.c`

```c
#include "mdns.h"

static bool s_mdns_running = false;

/* mdns_start_cb: mdns_free() if running (idempotent), mdns_init(),
   mdns_hostname_set(hostname). */

/* mdns_announce_cb: mdns_service_add(instance, service_type, proto,
   port, NULL, 0). */

/* mdns_query_cb: mdns_query_ptr(service_type, proto, timeout_ms, 5, results).
   Format each result as "hostname:port" into result fields. */

/* mdns_stop_cb: if (s_mdns_running) { mdns_free(); s_mdns_running = false; } */
```

### `jpp_app_dispatch.c`

In `teardown_sd_app()`, after the existing `jpp_sdk_net_close_all` call:

```c
if (ctx != NULL) {
    jpp_sdk_mdns_stop(ctx);
    jpp_sdk_http_server_stop(ctx);   /* see Step 12 */
}
```

### `jpp_mp_sdk_module.c`

```
jppsdk.mdns_start(hostname: str) -> None
jppsdk.mdns_announce(service_type: str, proto: str, instance: str, port: int) -> None
jppsdk.mdns_query(service_type: str, proto: str, timeout_ms: int) -> list[str]
    # ["hostname:port", ...]
jppsdk.mdns_stop() -> None
```

---

## Step 12 — `http.server` (tier-1, app-owned HTTP endpoint)

**`esp_http_server` is no longer linked.** WebDAV and the LRV server moved to
the in-house `jpp_http_server_core` (`components/jpp_core/`), which runs its
task stack and I/O buffer out of the shared 80 KB `jpp_app_pool` instead of the
heap. Two consequences for this proposal:

1. Reaching for `httpd_start()` here re-links the whole ESP-IDF HTTP server
   (~20 KB of flash on a budget with ~66 KB free). Prefer
   `jpp_http_server_core` — the handler-callback shape below maps onto it
   directly.
2. `jpp_http_server_start()` **acquires the app pool**, and a running app
   already holds it. An app-owned server therefore cannot use the core as-is:
   it needs a mode that takes its memory from the app's own pool allocation
   (the app knows how much of its 80 KB it can spare) rather than acquiring the
   pool itself. Design that before implementing this step. The upside is that
   the WebDAV/LRV mutual exclusion below stops being a policy check and becomes
   structural.

The bridge mechanism: two FreeRTOS queues of depth 1 connect the httpd task
(producer of requests, consumer of responses) to the app task (consumer of
requests, producer of responses). One request in flight at a time — subsequent
HTTP connections wait in the httpd socket queue.

### New types in `jpp_sdk_bridge.h`

```c
#define JPP_SDK_HTTP_SERVER_URI_MAX     64u
#define JPP_SDK_HTTP_SERVER_METHOD_MAX   8u
#define JPP_SDK_HTTP_SERVER_BODY_MAX  2048u
#define JPP_SDK_HTTP_SERVER_QUERY_MAX  256u

typedef struct {
    char method[JPP_SDK_HTTP_SERVER_METHOD_MAX];
    char uri[JPP_SDK_HTTP_SERVER_URI_MAX];
    char body[JPP_SDK_HTTP_SERVER_BODY_MAX];    /* empty for GET/HEAD */
    char query[JPP_SDK_HTTP_SERVER_QUERY_MAX];  /* raw query string */
} jpp_sdk_http_server_req_t;

typedef jpp_broker_status_t (*jpp_sdk_http_server_start_fn_t)(
    void *context, uint16_t port, jpp_broker_result_t *result);
typedef jpp_broker_status_t (*jpp_sdk_http_server_recv_fn_t)(
    void *context, uint32_t timeout_ms,
    jpp_sdk_http_server_req_t *out_req, jpp_broker_result_t *result);
typedef jpp_broker_status_t (*jpp_sdk_http_server_respond_fn_t)(
    void *context, uint16_t status_code,
    const char *content_type, const char *body, jpp_broker_result_t *result);
typedef void (*jpp_sdk_http_server_stop_fn_t)(void *context);

/* in jpp_sdk_native_services_t: */
jpp_sdk_http_server_start_fn_t   http_server_start;
jpp_sdk_http_server_recv_fn_t    http_server_recv;
jpp_sdk_http_server_respond_fn_t http_server_respond;
jpp_sdk_http_server_stop_fn_t    http_server_stop;
void                            *http_server_context;

/* Requires: http.server
   Start an HTTP server on port. Fails with BROKER_ERROR ("SERVER_ACTIVE") if
   WebDAV or the LRV server is already running. One server per app session. */
jpp_sdk_status_t jpp_sdk_http_server_start(jpp_sdk_context_t *ctx,
                                           uint16_t port,
                                           jpp_broker_result_t *result);

/* Requires: http.server
   Block until an HTTP request arrives or timeout_ms elapses.
   result->ok is true either way; check result->field_count > 0 to distinguish
   a real request from a timeout. If a request arrived, *out_req is filled.
   The app MUST call jpp_sdk_http_server_respond() before calling recv again —
   the httpd handler blocks until the response is sent. */
jpp_sdk_status_t jpp_sdk_http_server_recv(jpp_sdk_context_t *ctx,
                                          uint32_t timeout_ms,
                                          jpp_sdk_http_server_req_t *out_req,
                                          jpp_broker_result_t *result);

/* Requires: http.server — send a response to the in-flight request. */
jpp_sdk_status_t jpp_sdk_http_server_respond(jpp_sdk_context_t *ctx,
                                             uint16_t status_code,
                                             const char *content_type,
                                             const char *body,
                                             jpp_broker_result_t *result);

/* No cap required — stop the server. Called automatically at teardown.
   Drains both queues; safe to call when no server is running. */
void jpp_sdk_http_server_stop(jpp_sdk_context_t *ctx);
```

### `jpp_native_services.c`

Response struct (placed in the static file, not in a header):

```c
typedef struct {
    uint16_t status_code;
    char     content_type[64];
    char     body[JPP_SDK_HTTP_SERVER_BODY_MAX];
} http_resp_msg_t;

static httpd_handle_t  s_app_httpd  = NULL;
static QueueHandle_t   s_http_req_q  = NULL;   /* httpd task → app task */
static QueueHandle_t   s_http_resp_q = NULL;   /* app task → httpd task */
```

Queues created once in `jpp_native_services_init()`:

```c
s_http_req_q  = xQueueCreate(1, sizeof(jpp_sdk_http_server_req_t));
s_http_resp_q = xQueueCreate(1, sizeof(http_resp_msg_t));
```

Catch-all httpd URI handler (registered for `"/*"`, all methods):

```
1. Copy method string, URI, query string (httpd_req_get_url_query_str),
   body (httpd_req_recv up to BODY_MAX bytes) into a jpp_sdk_http_server_req_t.
2. xQueueSend(s_http_req_q, &req, pdMS_TO_TICKS(100)).
   If queue is full (app is not polling): respond 503 and return.
3. xQueueReceive(s_http_resp_q, &resp, pdMS_TO_TICKS(30000)).
   If timeout (app stalled): respond 504.
4. httpd_resp_set_status / httpd_resp_set_type / httpd_resp_send.
```

- `http_server_start_cb`: check WebDAV / LRV; start the server; register the
  handler. Written against `esp_http_server` above; see the note at the top of
  this step for why `jpp_http_server_core` is the better base and what has to
  change in it first. The `"LOW_HEAP"` guard (free heap <
  `JPP_HEAP_MON_WARN_BYTES`, 30 KB) only applies to the `httpd_start()`
  variant — a pool-backed server allocates nothing from the heap.
- `http_server_recv_cb`: `xQueueReceive(s_http_req_q, out_req, ticks)`. On
  timeout: `jpp_broker_ok_result(result)` with no additional fields. On
  receive: put result fields `"method"`, `"uri"`, `"query"`, `"body"`.
- `http_server_respond_cb`: fill `http_resp_msg_t`, `xQueueSend(s_http_resp_q, ...)`.
- `http_server_stop_cb`: `httpd_stop(s_app_httpd)`, drain both queues,
  `s_app_httpd = NULL`.

### `jpp_mp_sdk_module.c`

```
jppsdk.http_server_start(port: int) -> None
jppsdk.http_server_recv(timeout_ms: int) -> dict | None
    # dict keys: "method", "uri", "body", "query"; None on timeout
jppsdk.http_server_respond(status: int, content_type: str, body: str) -> None
jppsdk.http_server_stop() -> None
```

---

## Implementation order

| Group | Steps | Notes |
|---|---|---|
| **G0** | 0 | Lock budget and manifest registry — everything depends on this |
| **G1** | 1–4 | Pure bridge or trivial callbacks; no new CMakeLists changes |
| **G2** | 5–8 | One new callback each; step 5 needs `driver` in CMakeLists, step 8 needs `wifi_get_ip` |
| **G3** | 9 | Builds on existing TCP socket table; no new types |
| **G4** | 10 | New UDP socket table and callback set |
| **G5** | 11–12 | New components; `http.server` is the most complex (queue-bridge) |

Within each group, commit the MicroPython bindings (`jpp_mp_sdk_module.c`)
last — only after the C bridge function is testable with `testapp_native`
(`apps/testapp_native/`, built in-tree by `idf.py build`).

---

## Risks and constraints

**Lock limit**: increasing `BROKER_LOCK_LIMIT` from 8 → 12 enlarges
`jpp_broker_lock_set_t` by 4 × `sizeof(jpp_broker_lock_entry_t)` (~48 bytes).
This structure lives on the stack in `jpp_native_services_init`; the increase
is negligible.

**`system.info` field count**: exactly 8 fields in one `jpp_broker_result_t`
that has `FIELD_LIMIT = 8`. This is right at the limit. If a field ever needs
to be split (e.g. free heap broken into internal / SPIRAM), the limit must be
raised first.

**`http.server` heap cost**: the request and response structs together are
~4.5 KB of queue storage on the heap. The start callback guards against low
heap with `JPP_HEAP_MON_WARN_BYTES`, but this is advisory. WebDAV and LRV
cannot coexist with an app-owned server in any case — they hold the app pool,
and so does the running app (see the note at the top of this step).

**`http.server` recv/respond contract**: the app *must* call `respond` before
calling `recv` again. Failure to do so leaves the httpd handler blocked and the
server unresponsive until the app exits (which triggers `stop`, draining the
queues). Document this prominently in `docs/sdk-reference.md`.

**`net.mdns` hostname scope**: `mdns_hostname_set` is device-global and persists
until `mdns_free`. An app that sets a hostname changes what the device answers
to on the LAN for its entire session. Apps should use hostnames that include
their app ID to avoid surprising the user.

**Temperature sensor range**: `TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80)`
covers 10–80 °C. Readings outside this range degrade gracefully but are less
accurate. This is acceptable for a monitoring / diagnostic use case.

**`hardware.rng` entropy caveat**: `esp_random()` returns full hardware entropy
only when at least one RF subsystem is active. When all radios are off (pure
offline use) the output is seeded from ADC noise at boot and is not
cryptographically strong. Apps that need strong randomness (key generation,
nonces) should ensure WiFi or BLE is on, or accept the weaker source.

---

## Documentation obligations (non-deferrable per AGENTS.md)

Every group commit must include:

- **`docs/sdk-reference.md`**: new function entries with signatures, parameters,
  return behaviour, and error codes. Specifically:
  - `canvas.text`: character set (printable ASCII 0x20–0x7E), dimensions (5×8 + 1 gap).
  - `http.server`: the recv/respond sequencing contract.
  - `system.info`: all 8 field names and value semantics.
  - `hardware.rng`: entropy caveat when all radios are off.
- **`docs/manifest.md`**: new tier-1 capability strings `net.connect`, `net.udp`,
  `net.mdns`, `http.server` with tier annotation and usage notes.
- **`AGENTS.md`**: extend the `jpp_sdk_bridge` CODE MAP entry with new ungated
  surface items; add `system.info` to the `jpp_native_services` entry; update
  the `jpp_resource_budget` entry; add the `http.server` recv/respond contract
  to CONVENTIONS.
