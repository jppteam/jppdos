# BLE and ESP-NOW

Bluetooth LE comes in four separately-gated pieces — passive scanning,
advertising, the GATT client, and the GATT server — plus connectionless
ESP-NOW over the Wi-Fi radio. Declare only the ones you use: each is its own
capability and its own consent prompt.

## BLE scan

!!! warning "Capability: `ble.scan`."
    **Tier 1** — prompted once, then persisted.

### `ble_scan`

Perform a passive BLE scan and collect advertisement packets.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_scan(jpp_sdk_context_t *ctx,
                                   uint32_t duration_ms,
                                   jpp_sdk_ble_scan_result_t *results,
                                   size_t max_results,
                                   size_t *out_count,
                                   jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ble_scan(duration_ms: int) -> list[dict]
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `duration_ms` | How long to scan, in milliseconds. |
| `results` | C only: array to fill. |
| `max_results` | C only: capacity of `results`. Maximum 20. |
| `out_count` | C only: number of results written. |

**Returns:**
- C: `JPP_SDK_OK`; `*out_count` devices found; `results[i]` has `address`, `name`, `rssi`, `ad_data`, `ad_data_len`.
- Python: `list[dict]`, each with keys `address` (str `"AA:BB:CC:DD:EE:FF"`), `name` (str, empty if unnamed), `rssi` (int dBm), `ad_data` (bytes up to 31).

**C struct — `jpp_sdk_ble_scan_result_t`:**

```c
typedef struct {
    char    address[18];       /* "AA:BB:CC:DD:EE:FF\0" */
    char    name[32];          /* local name, or "" if none */
    int8_t  rssi;              /* signal strength in dBm */
    uint8_t ad_data[31];       /* raw advertisement payload */
    size_t  ad_data_len;       /* bytes used in ad_data */
} jpp_sdk_ble_scan_result_t;
```

---

## BLE advertise

!!! warning "Capability: `ble.advertise`."
    **Tier 1** — prompted once, then persisted. Also covers
    [`ble_set_connectable`](#ble_set_connectable).

### `ble_advertise_start`

Begin broadcasting a BLE advertisement payload.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_advertise_start(jpp_sdk_context_t *ctx,
                                              const uint8_t *payload,
                                              size_t payload_len,
                                              jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ble_advertise_start(payload: bytes) -> None
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `payload` | Raw advertisement payload. Maximum 31 bytes. |

---

### `ble_advertise_stop`

Stop broadcasting.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_advertise_stop(jpp_sdk_context_t *ctx,
                                             jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ble_advertise_stop() -> None
```
///

---

### `ble_set_connectable` { #ble_set_connectable }

Set whether inbound BLE connections are accepted while advertising.

**Capability:** `ble.advertise`

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_set_connectable(jpp_sdk_context_t *ctx,
                                              bool connectable,
                                              jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ble_set_connectable(connectable: bool) -> None
```
///

**Notes:** The default is non-connectable. A GATT-host app must call this with `True` before `ble_advertise_start` so a peer can open a connection.

---

## BLE GATT client

!!! warning "Capability: `ble.connect`."
    **Tier 2** — prompted on first use of every launch, never persisted.

Establish an outbound connection to a BLE peripheral and read/write its characteristics. Maximum 2 simultaneous connections per session.

!!! danger "One characteristic value is capped at 512 bytes."
    This is `BLE_ATT_ATTR_MAX_LEN`, a fixed Bluetooth spec limit, not a firmware
    tunable, and it applies in both directions. `ble_read_char` reassembles long
    values via ATT Read Blob and `ble_write_char` splits large writes via ATT
    Prepare/Execute, but neither can exceed 512 bytes for one value; a larger
    write is rejected by the peer and a read is truncated.

    To move a payload larger than that, chunk it at the app layer — see the
    reusable `jpp_ble_msg` helper (`apps/common/jpp_ble_msg.{c,h}`), which
    frames a message into ordered chunks over `ble_write_char` and reassembles
    them on the peer from [`ble_host_wait_write`](#ble_host_wait_write).

    Reconnecting to a peer works reliably across rounds: `ble_disconnect` blocks
    until the link is fully torn down, and a `ble.host` peripheral keeps
    advertising after a peer disconnects.

### `ble_connect`

Connect to a BLE peripheral by address.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_connect(jpp_sdk_context_t *ctx,
                                      const char *address,
                                      jpp_sdk_ble_conn_t *out_conn);
```
///

/// tab | MicroPython
```python
jppsdk.ble_connect(address: str) -> int
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `address` | BLE address string in format `"AA:BB:CC:DD:EE:FF"`. |
| `out_conn` / return | Connection handle for subsequent `ble_read_char`/`ble_write_char`/`ble_disconnect` calls. |

---

### `ble_read_char`

Read a GATT characteristic value.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_read_char(jpp_sdk_context_t *ctx,
                                        jpp_sdk_ble_conn_t conn,
                                        const char *svc_uuid,
                                        const char *char_uuid,
                                        jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ble_read_char(conn: int, svc_uuid: str, char_uuid: str) -> dict
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `svc_uuid` | Service UUID, full format `"XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"`. |
| `char_uuid` | Characteristic UUID, same format. |

**Returns:** `result->text` / `result["text"]` — the characteristic value as a string.

---

### `ble_write_char`

Write a value to a GATT characteristic.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_write_char(jpp_sdk_context_t *ctx,
                                         jpp_sdk_ble_conn_t conn,
                                         const char *svc_uuid,
                                         const char *char_uuid,
                                         const char *text,
                                         jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ble_write_char(conn: int, svc_uuid: str, char_uuid: str, text: str) -> None
```
///

---

### `ble_disconnect`

Close a BLE connection.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_disconnect(jpp_sdk_context_t *ctx,
                                         jpp_sdk_ble_conn_t conn);
```
///

/// tab | MicroPython
```python
jppsdk.ble_disconnect(conn: int) -> None
```
///

---

## BLE GATT server

!!! warning "Capability: `ble.host`."
    **Tier 2** — prompted on first use of every launch, never persisted.

The firmware pre-registers a generic app-host GATT service. An app claims it with `ble_service_register`, publishes data via the TX characteristic, and receives data via the RX characteristic. There are no per-app GATT definitions in the firmware — apps identify their protocol through the advertisement payload.

**Fixed UUIDs:**

| Symbol | UUID | Direction |
|--------|------|-----------|
| `JPP_SDK_BLE_HOST_SVC_UUID` | `4a505300-0000-0000-0000-000000000000` | Service |
| `JPP_SDK_BLE_HOST_TX_UUID` | `4a505301-0000-0000-0000-000000000000` | Readable by peer |
| `JPP_SDK_BLE_HOST_RX_UUID` | `4a505302-0000-0000-0000-000000000000` | Writable by peer |

### `ble_service_register`

Claim the firmware's app-host GATT service for this session.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_service_register(jpp_sdk_context_t *ctx,
                                               const char *svc_uuid,
                                               jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ble_service_register(svc_uuid: str) -> None
```
///

**Notes:** Pass `JPP_SDK_BLE_HOST_SVC_UUID` as the UUID. Only one app can claim the service at a time.

---

### `ble_service_unregister`

Release the GATT service.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_service_unregister(jpp_sdk_context_t *ctx,
                                                  jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ble_service_unregister() -> None
```
///

---

### `ble_host_set_value` { #ble_host_set_value }

Publish bytes to the TX characteristic so connected peers can read them.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_host_set_value(jpp_sdk_context_t *ctx,
                                              const uint8_t *data,
                                              size_t len,
                                              jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ble_host_set_value(data: bytes) -> None
```
///

**Notes:** At most `JPP_SDK_BLE_HOST_RX_MAX` (4096) bytes per call; a longer `data` raises `ValueError` in Python.

---

### `ble_host_wait_write` { #ble_host_wait_write }

Block until a peer writes to the RX characteristic, or the timeout elapses.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_host_wait_write(jpp_sdk_context_t *ctx,
                                              uint8_t *buf,
                                              size_t *len_inout,
                                              uint32_t timeout_ms,
                                              bool *out_received,
                                              jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ble_host_wait_write(timeout_ms: int) -> bytes | None
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `buf` | C only: buffer to receive the written bytes. |
| `len_inout` | C only: in — buffer capacity; out — bytes received. |
| `timeout_ms` | Maximum wait time. `0` waits forever. |
| `out_received` | C only: set to `true` if a write was received within the timeout. |

**Returns:**

- C: status, with the payload in `buf` and `*out_received` telling you whether a write arrived.
- Python: the received `bytes`, or `None` if the timeout elapsed with no write.

---

### `ble_host_clear` { #ble_host_clear }

Clear the RX write buffer.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ble_host_clear(jpp_sdk_context_t *ctx,
                                         jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ble_host_clear() -> None
```
///

---

## ESP-NOW

!!! warning "Capability: `esp_now`."
    **Tier 1** — prompted once, then persisted.

Connectionless peer-to-peer messaging over WiFi. Shares the STA-mode WiFi radio with `http.request`, WebDAV, and the LRV server.

### `espnow_send`

Send data to a peer, identified by its 6-byte MAC address. The peer is added to the ESP-NOW peer list automatically on first use (open, unencrypted, current WiFi channel). Blocks until the send completes or an internal timeout elapses.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_espnow_send(jpp_sdk_context_t *ctx,
                                      const uint8_t peer_mac[6],
                                      const uint8_t *data,
                                      size_t data_len,
                                      jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.espnow_send(peer_mac: bytes, data: bytes) -> None
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `peer_mac` | 6-byte peer MAC address. |
| `data` | Payload to send. Maximum 250 bytes. |

---

### `espnow_recv`

Wait up to `timeout_ms` for an incoming ESP-NOW packet.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_espnow_recv(jpp_sdk_context_t *ctx,
                                      uint8_t out_peer_mac[6],
                                      uint8_t *out_data,
                                      size_t max_len,
                                      size_t *out_len,
                                      uint32_t timeout_ms,
                                      jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.espnow_recv(timeout_ms: int) -> tuple[bytes, bytes] | None
```
///

**Returns:**
- C: `JPP_SDK_STATUS_OK` with `out_peer_mac`/`out_data`/`out_len` filled on a received packet; `JPP_SDK_STATUS_NO_DATA` (not an error) if nothing arrived within `timeout_ms`.
- Python: `(peer_mac, data)` tuple of `bytes`, or `None` on timeout.

**Notes:** Received packets are buffered in a small internal queue (depth 8); a packet is dropped if the app doesn't call `espnow_recv` often enough to drain it.

---
