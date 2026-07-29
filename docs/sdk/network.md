# Network

Four transports, each behind its own capability: cleartext HTTP, TLS-verified
HTTPS, an inbound TCP listener, and an outbound TCP client. All of them need
Wi-Fi to be connected, and the socket calls additionally fail while the WebDAV
or LRV HTTP server is running.

## HTTP

!!! warning "Capability: `http.request`."
    **Tier 1** — the user is prompted once and the grant is persisted to
    `/data/grants/<app_id>.json`. Declare it in
    [`manifest.json`](../manifest.md#capabilities).

### `http_request`

Make a synchronous HTTP GET or POST request.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_http_request(jpp_sdk_context_t *ctx,
                                       const char *method,
                                       const char *url,
                                       const char *body,
                                       jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.http_request(method: str, url: str, body: str | None = None) -> dict
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `method` | `"GET"` or `"POST"`. |
| `url` | Full URL including scheme, e.g. `"http://192.168.1.1/api/data"`. |
| `body` | Request body for POST. Pass `NULL`/`None` for GET. |

**Returns:**
- C: `JPP_SDK_OK`; `result->status_code` is the HTTP status (e.g. `"200"`) and `result->body` is the response body.
- Python: `{"status_code": int, "body": str}`.

**Notes:**
- HTTP requests are serialized through the broker — only one can be in flight at a time.
- Cleartext only. For `https://` URLs use [`https_request`](#https_request), which is a separate capability.
- Wi-Fi must be connected; the request fails with a broker error if the network is unavailable.

---

## HTTPS

!!! warning "Capability: `https.request` — plus a one-time prompt per origin."
    **Tier 1** for the capability itself, then a *second* prompt naming each new
    origin the app contacts. See [Per-origin consent](#per-origin-consent).

!!! info "Requires SDK level 2."
    Declare `"sdk_min": 2`. Firmware v1.0-RTM refuses to load the app —
    see the [SDK changelog](../sdk-changelog.md).

### `https_request`

Make a synchronous HTTPS GET or POST request, with the server's certificate verified against the CA roots built into the firmware.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_https_request(jpp_sdk_context_t *ctx,
                                        const char *method,
                                        const char *url,
                                        const char *body,
                                        jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.https_request(method: str, url: str, body: str | None = None) -> dict
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `method` | `"GET"` or `"POST"`. |
| `url` | Full URL, which **must** start with `https://` — e.g. `"https://api.example.com/v1/status"`. |
| `body` | Request body for POST, up to 2048 bytes. Pass `NULL`/`None` for GET. |

**Returns:**
- C: `JPP_SDK_OK`; `result->status_code` is the HTTP status (e.g. `"200"`) and `result->body` is the response body.
- Python: `{"status_code": int, "body": str}`.

#### Per-origin consent

The capability grant alone does not let an app reach the whole web. Consent is scoped to an **origin** — the `scheme://host[:port]` part of the URL — and the user approves each one separately:

1. First ever `https_request` call: the usual capability prompt (*"make secure (HTTPS) web requests"*), persisted like any tier-1 grant.
2. First call to each **new origin**: a second prompt naming that origin (*"Connect securely to: api.example.com"*). An allowed origin is appended to `/data/grants/<app_id>.origins` and never asked about again.
3. Every later call to an already-approved origin goes straight through with no prompt.

So an app that talks to one server prompts twice, once, and is then silent; an app that starts contacting a *different* host has to ask the user again. Denial returns `JPP_SDK_ACCESS_DENIED` and no request is sent.

Origins are normalised before comparison: the host is lowercased and an explicit `:443` is dropped, so `https://API.Example.com` and `https://api.example.com:443` are the same grant.

**Notes:**
- Shares the broker's HTTP lock with `http_request` — one outbound request in flight at a time, whichever transport.
- Certificate verification cannot be disabled. There is no "insecure" flag in the SDK, and a chain that does not validate fails the request.
- A failure before the first response byte — DNS, TCP, TLS handshake, or a rejected certificate — all surface as the same `HTTPS_CONNECT_FAILED` error, because `esp_http_client` does not distinguish them to its caller. The specific cause is on the serial log as `HTTPS_FAILED <esp_err_name>` under tag `native_svc`; check there when a URL that works in a browser fails here.
- The trust store is the ESP-IDF *common* CA bundle: 43 roots covering Amazon, DigiCert, GlobalSign, GoDaddy, Google Trust Services, IdenTrust, ISRG (Let's Encrypt) and Sectigo. A server whose chain roots elsewhere will fail even though it works in a desktop browser.
- URLs are rejected (`JPP_SDK_INVALID_ARGUMENT`, `BAD_URL`) if the scheme is not `https`, if they carry userinfo (`https://user@host/`), or if the host is an IPv6 literal. Userinfo is refused specifically because `https://trusted.example@evil.example/` reads as one host and contacts another.
- TLS costs roughly 20 KB of heap for the handshake. During background (headless) runs there is no way to show a prompt, so only origins already approved interactively will work.
- Wi-Fi must be connected; the request fails with a broker error if the network is unavailable.

---

## TCP server

!!! warning "Capability: `network.bind`."
    **Tier 2** — the user is prompted on first use of *every* launch and the
    answer is never persisted.

One listener socket, up to 2 accepted connections. Binding fails while the WebDAV or LRV server is running. All sockets close automatically when the app exits.

### `net_bind`

Open a TCP listener on the specified port.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_net_bind(jpp_sdk_context_t *ctx,
                                   uint16_t port,
                                   jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.net_bind(port: int) -> None
```
///

**Returns:** `JPP_SDK_OK`; `result->port` is the bound port number (as a string).

---

### `net_accept`

Wait for an incoming connection. Returns a socket id, or `NULL`/`-1` on timeout.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_net_accept(jpp_sdk_context_t *ctx,
                                     uint32_t timeout_ms,
                                     int *out_sock,
                                     jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.net_accept(timeout_ms: int) -> int | None
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `timeout_ms` | How long to wait for a connection. Pass `0` to wait indefinitely. |
| `out_sock` / return | Socket identifier for `net_recv`/`net_send`/`net_close`. `-1` if timed out. |

---

### `net_recv`

Receive up to `max_len` bytes from a connected socket.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_net_recv(jpp_sdk_context_t *ctx,
                                   int sock,
                                   uint8_t *buf,
                                   size_t buf_len,
                                   size_t *out_len,
                                   uint32_t timeout_ms,
                                   jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.net_recv(sock: int, max_len: int, timeout_ms: int) -> bytes
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `sock` | Socket id from `net_accept`. |
| `buf_len` / `max_len` | Maximum bytes to read. Hard limit: 1024 bytes per call. |
| `timeout_ms` | Receive timeout. Pass `0` to wait indefinitely. |

**Returns:**
- C: `JPP_SDK_OK`; `*out_len` is bytes received (0 on timeout); `result->closed` is `"1"` if the peer closed the connection.
- Python: `bytes` — empty `b""` on timeout or peer close.

---

### `net_send`

Send bytes to a connected socket.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_net_send(jpp_sdk_context_t *ctx,
                                   int sock,
                                   const uint8_t *data,
                                   size_t len,
                                   jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.net_send(sock: int, data: bytes) -> None
```
///

---

### `net_close`

Close a socket. Pass `sock = -1` to close the listener (without closing accepted connections).

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_net_close(jpp_sdk_context_t *ctx,
                                    int sock,
                                    jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.net_close(sock: int) -> None
```
///

**Notes:** Close accepted sockets when done — the connection table has only 2 slots. Close the listener (`sock=-1`) when you no longer want new connections.

---

## TCP client

!!! warning "Capability: `network.connect`."
    **Tier 2** — prompted on first use of every launch, never persisted.

!!! info "Requires SDK level 2 · C only."
    Declare `"sdk_min": 2`. There is no `jppsdk.net_connect` binding, so
    MicroPython apps are limited to the [TCP server](#tcp-server) side.

Open an outbound TCP connection. A connected socket is used with the same
`net_recv` / `net_send` / `net_close` calls as an accepted server socket, and
occupies a slot in the same 2-entry connection table. `net_recv`/`net_send`/
`net_close` accept a socket obtained from **either** `net_bind`+`net_accept`
(`network.bind`) **or** `net_connect` (`network.connect`) — holding either
capability is sufficient to move bytes on a socket you own.

### `net_connect` (C only) { #net_connect }

Resolve `host` (DNS name or dotted-quad) and connect to `port`.

```c
jpp_sdk_status_t jpp_sdk_net_connect(jpp_sdk_context_t *ctx,
                                     const char *host,
                                     uint16_t port,
                                     uint32_t timeout_ms,
                                     int *out_sock,
                                     jpp_broker_result_t *result);
```

| Parameter | Meaning |
|-----------|---------|
| `host` | Hostname or IPv4 literal to connect to. |
| `port` | TCP port. |
| `timeout_ms` | Connect timeout. `0` blocks until the OS gives up. |
| `out_sock` / return | Socket id for `net_recv`/`net_send`/`net_close`; `-1` on failure. |

**Returns:** `JPP_SDK_OK` with `*out_sock ≥ 0` on success. `result->code` is
`CONNECT_FAILED` (DNS/connect error or timeout), `SOCKET_LIMIT` (connection
table full), or `SERVER_ACTIVE` (the WebDAV or LRV HTTP server is running).

**Notes:** Wi-Fi must be connected. Binary-safe in both directions (unlike
`http_request`, which is HTTP-only and NUL-terminates the body). Close the
socket with `net_close` when done; all sockets close automatically at app exit.

---
