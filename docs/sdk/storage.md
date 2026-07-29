# Storage and IPC

Four ways to persist data, in rough order of how often you should reach for
them: the [key-value store](#key-value-store) for small state, [scoped file
I/O](#scoped-file-io) for the app's own files, [shared file
I/O](#shared-file-io) for files other apps may read, and [full file
access](#full-file-access) for anything else on the card — the only one that
needs a capability. [IPC](#ipc) delivers short text messages between apps.

!!! success "The sandbox is free."
    Everything on this page except full file access is **ungated**: no manifest
    declaration, no consent prompt. Scoping is enforced by path construction, so
    an app physically cannot name a file outside its own directory.

## Scoped file I/O

These calls are sandboxed to the app's own directory: `/sd/apps/<app_id>/`. Paths are **relative** — a path of `"data/notes.txt"` accesses `/sd/apps/<app_id>/data/notes.txt`. Parent directory traversal (`../`) is rejected.

**Capability:** None

### `file_read`

Read the entire contents of a file into memory.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_file_read(jpp_sdk_context_t *ctx,
                                    const char *relative_path,
                                    jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.file_read(path: str) -> dict
```
///

**Returns:**
- C: `JPP_SDK_OK`; `result->text` is the file contents as a null-terminated string.
- Python: `{"text": str}` — access the contents with `result["text"]`.

---

### `file_write`

Write a string to a file, replacing its contents. Creates the file (and any parent directories) if necessary.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_file_write(jpp_sdk_context_t *ctx,
                                     const char *relative_path,
                                     const char *text,
                                     jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.file_write(path: str, text: str) -> None
```
///

**Notes:** Writing is atomic via a temp-file rename — a power loss during the write will not corrupt the existing file.

---

### `file_list`

List the contents of a directory.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_file_list(jpp_sdk_context_t *ctx,
                                    const char *relative_dir,
                                    jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.file_list(dir: str) -> dict
```
///

**Returns:**
- C: `JPP_SDK_OK`; `result->text` is a newline-separated list of filenames. Individual entries are also in `result` as `entry_0`, `entry_1`, etc.
- Python: `{"text": str}` — split on `"\n"` to get individual names.

---

## Shared file I/O

These calls work identically to scoped file I/O but target `/sd/shared/<app_id>/` instead. Files here can be seen and read by other apps that specifically open that path with `files.full`.

**Capability:** None

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_shared_read(jpp_sdk_context_t *ctx,
                                      const char *relative_path,
                                      jpp_broker_result_t *result);

jpp_sdk_status_t jpp_sdk_shared_write(jpp_sdk_context_t *ctx,
                                       const char *relative_path,
                                       const char *text,
                                       jpp_broker_result_t *result);

jpp_sdk_status_t jpp_sdk_shared_list(jpp_sdk_context_t *ctx,
                                      const char *relative_dir,
                                      jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.shared_read(path: str) -> dict
jppsdk.shared_write(path: str, text: str) -> None
jppsdk.shared_list(dir: str) -> dict
```
///

---

## Full file access

These calls allow access to **any path on the SD card** under `/sd/`. Each path the app opens requires individual user approval — a Deny/Allow prompt appears before the handle is granted.

!!! warning "Capability: `files.full` — plus a prompt per path."
    **Tier 2** — prompted on first use of every launch, never persisted. On top
    of that, *every individual path* the app opens raises its own Deny/Allow
    prompt, which is likewise never persisted. The capability answers "may this
    app touch the filesystem at all"; the path prompt answers "which file",
    which is the question a user can actually judge.

### `file_open`

Open a file and obtain a handle. A user approval prompt is shown for the requested path and mode before the handle is granted.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_file_open(jpp_sdk_context_t *ctx,
                                    const char *path,
                                    jpp_sdk_open_mode_t mode,
                                    jpp_sdk_handle_t *out_handle);
```
///

/// tab | MicroPython
```python
jppsdk.file_open(path: str, mode: int) -> int
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `path` | Absolute path under `/sd/`, e.g. `"/sd/shared/other_app/data.txt"`. |
| `mode` | `OPEN_READ` (0) or `OPEN_WRITE` (1). |
| `out_handle` / return | An integer handle used in subsequent `handle_*` calls. |

**Limits:** Maximum 4 open handles per session. Returns `JPP_SDK_HANDLE_LIMIT` if exceeded.

---

### `handle_read`

Read the entire contents of an open file handle.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_handle_read(jpp_sdk_context_t *ctx,
                                      jpp_sdk_handle_t handle,
                                      jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.handle_read(handle: int) -> dict
```
///

**Returns:** `result->text` / `result["text"]` — the file contents.

---

### `handle_write`

Write a string to an open file handle (must be opened with `OPEN_WRITE`).

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_handle_write(jpp_sdk_context_t *ctx,
                                       jpp_sdk_handle_t handle,
                                       const char *text,
                                       jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.handle_write(handle: int, text: str) -> None
```
///

---

### `handle_list`

List directory contents for a handle opened on a directory path.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_handle_list(jpp_sdk_context_t *ctx,
                                      jpp_sdk_handle_t handle,
                                      jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.handle_list(handle: int) -> dict
```
///

---

### `handle_close`

Close an open handle and release the slot.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_handle_close(jpp_sdk_context_t *ctx,
                                       jpp_sdk_handle_t handle);
```
///

/// tab | MicroPython
```python
jppsdk.handle_close(handle: int) -> None
```
///

**Notes:** All open handles are automatically closed when the app exits. Close handles promptly — the limit is 4 per session.

---

## Key-value store

A simple persistent key-value store backed by `.kv.json` in the app's scoped directory (`/sd/apps/<app_id>/.kv.json`). Survives app restarts; suitable for preferences, scores, cached data.

**Capability:** None

### `kv_get`

Return the value for a key, or `None`/`JPP_SDK_NO_DATA` if the key does not exist.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_kv_get(jpp_sdk_context_t *ctx,
                                 const char *key,
                                 char *out_value,
                                 size_t value_buf_len);
```
///

/// tab | MicroPython
```python
jppsdk.kv_get(key: str) -> str | None
```
///

**Parameters:**

| Name | Limit |
|------|-------|
| Key | 64 characters |
| Value | 256 characters |

**Returns:**
- C: `JPP_SDK_OK` if the key exists, `JPP_SDK_NO_DATA` if absent.
- Python: the value string, or `None` if the key does not exist (does not raise).

---

### `kv_set`

Set or update a key.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_kv_set(jpp_sdk_context_t *ctx,
                                 const char *key,
                                 const char *value);
```
///

/// tab | MicroPython
```python
jppsdk.kv_set(key: str, value: str) -> None
```
///

---

### `kv_delete`

Delete a key. No-op if the key does not exist.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_kv_delete(jpp_sdk_context_t *ctx,
                                    const char *key);
```
///

/// tab | MicroPython
```python
jppsdk.kv_delete(key: str) -> None
```
///

---

## IPC

Apps can send and receive text messages through file-backed mailboxes at `/sd/ipc/<recipient>/<sender>/`. Messages are consumed and deleted on receive.

**Capability:** None

### `ipc_send`

Send a message to another app's mailbox.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ipc_send(jpp_sdk_context_t *ctx,
                                   const char *recipient_app_id,
                                   const char *payload,
                                   jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ipc_send(recipient_id: str, payload: str) -> None
```
///

**Parameters:**

| Name | Description |
|------|-------------|
| `recipient_app_id` | The `app_id` of the target app. The target does not need to be running. |
| `payload` | Message body. Maximum 512 bytes. |

---

### `ipc_recv`

Check this app's mailbox and return the oldest unread message. Non-blocking.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_ipc_recv(jpp_sdk_context_t *ctx,
                                   char *out_payload,
                                   size_t payload_buf_len,
                                   jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.ipc_recv() -> tuple[str, str] | None
```
///

**Returns:**
- C: `JPP_SDK_OK` if a message was read; `JPP_SDK_NO_DATA` if the mailbox is empty. When OK, `result->text` is the payload and `result->sender` is the sender's `app_id`.
- Python: `(payload, sender_app_id)` tuple, or `None` if no messages.

**Notes:** Messages are deleted from the mailbox on receipt. Poll `ipc_recv()` in `on_idle` to process incoming messages.

---
