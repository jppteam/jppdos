# JPPD-SMP Serial Protocol Reference

**J++Device Serial Management Protocol v1** — a binary protocol over USB/UART for host-side tools (PC clients, the JPPD desktop app) to manage files on the SD card, query device information, and retrieve LRV verification data.

This document is for authors of **host-side tooling**. It is not needed to write apps.

---

## Physical layer

| Parameter | Value |
|-----------|-------|
| Interface | UART0 (exposed over USB) |
| Baud rate | 115200 |
| Format | 8N1 |
| Coexistence | ESP_LOG text output shares the same channel; the device uses a TX mutex so log bytes never interleave with binary frame bytes |

---

## Frame format

Every message — in both directions — is wrapped in the same envelope:

```
[SOF    4 B]   0x01 0x4A 0x50 0x50  ("\x01JPP")
[LEN    2 B]   payload byte count, little-endian
[PAYLOAD  …]   LEN bytes
[CRC    2 B]   CRC-16/CCITT-FALSE over LEN(2) + PAYLOAD(LEN), little-endian
```

**CRC-16/CCITT-FALSE** parameters: polynomial `0x1021`, initial value `0xFFFF`, no input/output reflection, no final XOR. The CRC covers the two LEN bytes followed by the payload bytes.

---

## Payload format

**Command (host → device):**

```
[SEQ    1 B]   sequence number — echoed in the response
[CMD    1 B]   command byte
[FLAGS  1 B]   reserved, must be 0x00
[BODY   …  ]   command-specific body (may be empty)
```

**Response (device → host):**

```
[SEQ    1 B]   echoed from the command
[STATUS 1 B]   result code (see Status codes)
[BODY   …  ]   command-specific body (present only on OK responses unless noted)
```

---

## Session lifecycle

A host must open a session before issuing any command other than `SESSION_START`.

1. Host sends `SESSION_START`.
2. The device displays an OLED consent dialog (**Allow / Deny**). The host blocks until the user responds.
3. If the user allows, `SESSION_START` returns `OK` and commands may flow.
4. If the user denies, `SESSION_START` returns `ERR_DENIED`. No session is opened.
5. The host sends `SESSION_END` when done, or the session times out after **30 seconds** of inactivity.

**Mutual exclusion:**
- A session cannot be opened while an SD app is running (`ERR_APP_RUNNING`).
- SD app launch is blocked while a session is open.
- Deep sleep is suppressed for the duration of the consent dialog and any active session.

All commands except `SESSION_START` return `ERR_NO_SESSION` if no session is open.

---

## Commands

### 0x00 — SESSION_START

Opens a management session. Triggers the on-device consent dialog.

| Direction | Body |
|-----------|------|
| Host → device | `[proto_ver: 1 B]` — must be `0x01` |
| Device → host | `[proto_ver: 1 B]` — echoes `0x01` |

Returns `ERR_DENIED` if the user denies, `ERR_BUSY` if a session is already open, `ERR_APP_RUNNING` if an app is currently running.

---

### 0x01 — SESSION_END

Closes the current session.

| Direction | Body |
|-----------|------|
| Host → device | — |
| Device → host | — |

---

### 0x02 — GET_INFO

Returns the firmware version string.

| Direction | Body |
|-----------|------|
| Host → device | — |
| Device → host | `[fw_version: NUL-terminated string]` |

---

### 0x03 — GET_LRV_DATA

Returns Limited Run Verification data for the certificate verification flow. Requires LRV data to be unlocked on the device (Settings › \* Device Info \* › enter password). Returns `ERR_DENIED` if locked.

| Direction | Body |
|-----------|------|
| Host → device | — |
| Device → host | `[cert: NUL-terminated]` `[cert_sig: 64 B]` `[device_pubkey: 32 B]` `[challenge: NUL-terminated]` `[resp_sig: 64 B]` |

**Challenge format:** `{username}|{YYYY-MM-DDTHH:MM:SSZ}` where `username` is taken from the device's stored user name and the timestamp is the device RTC at the moment of the request.

`resp_sig` is a 64-byte raw Ed25519 signature of the challenge bytes using the device's private key.

---

### 0x10 — FS_LIST_DIR

Lists the contents of a directory on the SD card. All paths must start with `/sd`.

| Direction | Body |
|-----------|------|
| Host → device | `[path: NUL-terminated]` |
| Device → host | `[count: 2 B LE]` then N × `[flags: 1 B][name: NUL-terminated]` |

**Flags byte:** bit 0 = `1` for directory, `0` for file. Bits 1–7 are reserved.

---

### 0x11 — FS_MKDIR

Creates a directory (and any missing parent directories) on the SD card.

| Direction | Body |
|-----------|------|
| Host → device | `[path: NUL-terminated]` |
| Device → host | — |

---

### 0x12 — FS_REMOVE

Removes a file or empty directory.

| Direction | Body |
|-----------|------|
| Host → device | `[path: NUL-terminated]` |
| Device → host | — |

---

### 0x13 — FS_RENAME

Renames or moves a file or directory within `/sd`.

| Direction | Body |
|-----------|------|
| Host → device | `[src: NUL-terminated][dst: NUL-terminated]` |
| Device → host | — |

---

### File upload (0x14 – 0x16)

Upload a file to the SD card in three steps:

**0x14 — FS_UPLOAD_BEGIN**

| Direction | Body |
|-----------|------|
| Host → device | `[file_size: 4 B LE][path: NUL-terminated]` |
| Device → host | `[xfer_id: 1 B]` — always `0x00` in v1 |

**0x15 — FS_UPLOAD_CHUNK**

Send up to 1024 bytes at a time. Chunks are zero-indexed.

| Direction | Body |
|-----------|------|
| Host → device | `[xfer_id: 1 B][chunk_idx: 2 B LE][data…]` |
| Device → host | `[chunk_idx: 2 B LE]` — echoed on success |

**0x16 — FS_UPLOAD_END**

Finalises the transfer and verifies integrity.

| Direction | Body |
|-----------|------|
| Host → device | `[xfer_id: 1 B][crc32: 4 B LE]` |
| Device → host | — |

The `crc32` field uses **CRC-32/ISO-HDLC** (`~crc32(~0, data, len)`), matching Python's `zlib.crc32` output. The device verifies the complete file against this value before committing.

---

### File download (0x17 – 0x19)

Download a file from the SD card in three steps:

**0x17 — FS_DOWNLOAD_BEGIN**

| Direction | Body |
|-----------|------|
| Host → device | `[path: NUL-terminated]` |
| Device → host | `[xfer_id: 1 B][file_size: 4 B LE][chunk_count: 2 B LE][crc32: 4 B LE]` |

The `crc32` is computed over the entire file (same algorithm as upload). The host should verify it after receiving all chunks.

**0x18 — FS_DOWNLOAD_CHUNK**

Request chunks by index (zero-based, up to 1024 bytes each).

| Direction | Body |
|-----------|------|
| Host → device | `[xfer_id: 1 B][chunk_idx: 2 B LE]` |
| Device → host | `[xfer_id: 1 B][chunk_idx: 2 B LE][data…]` |

**0x19 — FS_DOWNLOAD_END**

Signals the host is done with the transfer.

| Direction | Body |
|-----------|------|
| Host → device | `[xfer_id: 1 B]` |
| Device → host | — |

---

## Status codes

| Code | Name | Meaning |
|------|------|---------|
| 0x00 | OK | Success |
| 0x01 | ERR_DENIED | User denied consent, or LRV data is locked |
| 0x02 | ERR_NOT_FOUND | File or directory does not exist |
| 0x03 | ERR_IO | SD card or filesystem error |
| 0x04 | ERR_EXISTS | Target already exists (rename/mkdir collision) |
| 0x05 | ERR_INVALID | Malformed request (bad path, unknown command, wrong proto version) |
| 0x06 | ERR_BUSY | Session already open |
| 0x07 | ERR_NO_SESSION | Command sent without an open session |
| 0x08 | ERR_TRANSFER | Chunk out of order, transfer ID mismatch, or CRC mismatch on end |
| 0x09 | ERR_OVERFLOW | Response too large for internal buffer |
| 0x0A | ERR_APP_RUNNING | Session cannot open while an app is running |

---

## Transfer rules

- Only one active transfer at a time. Starting a new `FS_UPLOAD_BEGIN` or `FS_DOWNLOAD_BEGIN` while a transfer is in progress returns `ERR_BUSY`.
- `xfer_id` is always `0x00` in protocol version 1.
- Chunk size is 1024 bytes. The final chunk may be smaller.
- File paths are restricted to the `/sd` tree. Paths outside `/sd` return `ERR_INVALID`.

---

## Example: uploading a file

```
HOST → SESSION_START   (proto_ver=0x01)
DEV  → OK             (proto_ver=0x01, user pressed Allow)

HOST → FS_UPLOAD_BEGIN (file_size=2048, path="/sd/apps/myapp/main.mpy")
DEV  → OK             (xfer_id=0x00)

HOST → FS_UPLOAD_CHUNK (xfer_id=0x00, chunk_idx=0, data[0..1023])
DEV  → OK             (chunk_idx=0)

HOST → FS_UPLOAD_CHUNK (xfer_id=0x00, chunk_idx=1, data[1024..2047])
DEV  → OK             (chunk_idx=1)

HOST → FS_UPLOAD_END   (xfer_id=0x00, crc32=<zlib.crc32 of full file>)
DEV  → OK

HOST → SESSION_END
DEV  → OK
```
