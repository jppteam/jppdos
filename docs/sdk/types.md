# Types and constants

### C — `jpp_sdk_status_t`

| Constant | Value | Meaning |
|----------|-------|---------|
| `JPP_SDK_OK` | 0 | Success |
| `JPP_SDK_INVALID_ARGUMENT` | 1 | Bad parameter (NULL, out of range, etc.) |
| `JPP_SDK_INVALID_STATE` | 2 | Called in the wrong state (e.g. no active session) |
| `JPP_SDK_ACCESS_DENIED` | 3 | Capability not granted — handle gracefully, do not crash |
| `JPP_SDK_TEXT_TRUNCATED` | 4 | Output was written but clipped to fit |
| `JPP_SDK_BROKER_ERROR` | 5 | Internal broker failure |
| `JPP_SDK_HANDLE_LIMIT` | 6 | Maximum number of open file handles reached (limit: 4) |
| `JPP_SDK_INVALID_HANDLE` | 7 | Handle was not opened or has been closed |
| `JPP_SDK_BLE_CONN_LIMIT` | 8 | Maximum BLE connections reached |
| `JPP_SDK_INVALID_BLE_CONN` | 9 | BLE connection handle is not valid |
| `JPP_SDK_NO_DATA` | 10 | Requested item does not exist (absent KV key, empty IPC mailbox) |

### C — `jpp_sdk_key_event_t`

| Constant | Meaning |
|----------|---------|
| `JPP_SDK_KEY_NONE` | No key (poll returned empty, or wait timed out) |
| `JPP_SDK_KEY_UP` | D-pad up |
| `JPP_SDK_KEY_DOWN` | D-pad down |
| `JPP_SDK_KEY_LEFT` | D-pad left |
| `JPP_SDK_KEY_RIGHT` | D-pad right |
| `JPP_SDK_KEY_OK` | D-pad OK (short press) |
| `JPP_SDK_KEY_BACK` | The user asked to go back. Which physical gesture produced it (hold or double-click) depends on Settings > Personalisation and is not your app's concern. |
| `JPP_SDK_KEY_OK_LONG` | Older name for `JPP_SDK_KEY_BACK`, same value — kept so existing apps are unaffected |
| `JPP_SDK_KEY_OK_HOLD` | Raw OK hold. Delivered only if claimed via [`claim_ok`](app-control.md#claim_ok) |
| `JPP_SDK_KEY_OK_DOUBLE` | Raw OK double-click. Delivered only if claimed via [`claim_ok`](app-control.md#claim_ok) |

### Python — `jppsdk` constants

```python
# Key events (match C enum values)
KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_OK
KEY_BACK, KEY_OK_LONG          # same value; KEY_BACK is preferred
KEY_OK_HOLD, KEY_OK_DOUBLE # raw gestures, only if claimed

# OK gesture claims (see claim_ok)
OK_CLAIM_NONE = 0
OK_CLAIM_HOLD = 1
OK_CLAIM_DOUBLE = 2

# File open modes
OPEN_READ = 0
OPEN_WRITE = 1

# Input types
INPUT_TEXT = 0    # On-screen QWERTY keyboard
INPUT_NUMBER = 1  # Digit keyboard with - and .
INPUT_DATE = 2    # Field spinner, returns "YYYY-MM-DD"
INPUT_TIME = 3    # Field spinner, returns "HH:MM:SS"

# Predefined buzzer sounds
SOUND_SUCCESS, SOUND_FAILURE, SOUND_NOTIFY, SOUND_STARTUP, SOUND_CLICK
```

### C — `jpp_broker_result_t`

Several SDK calls return results through a `jpp_broker_result_t *result` output parameter. This struct carries key-value pairs from the broker:

| Field | Set by | Description |
|-------|--------|-------------|
| `text` | File I/O, `get_time`, directory listing | The primary text payload |
| `status_code` | `http_request`, `https_request` | HTTP response status code (int as string) |
| `body` | `http_request`, `https_request` | HTTP response body |
| `port` | `net_bind` | The bound port number |
| `closed` | `net_recv` | Set to `"1"` if the peer closed the connection |
| `sender` | `ipc_recv` | The sender's `app_id` |

Access fields with `jpp_broker_result_get(result, "text")`, which returns a `const char *` or `NULL` if the field is absent.

---
