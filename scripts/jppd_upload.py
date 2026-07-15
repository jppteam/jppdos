#!/usr/bin/env python3
"""
jppd_upload.py — Upload app artifacts to a JPP device via JPPD-SMP.

Usage:
    python3 scripts/jppd_upload.py <port> <app_id> [build_dir]

Arguments:
    port      Serial port (e.g. /dev/ttyUSB0 or /dev/cu.usbserial-...)
    app_id    App ID matching the manifest (e.g. games, meetapp)
    build_dir Build output directory (default: build/apps/<app_id>)

The device shows a consent dialog when the session opens — press Allow on the
device to proceed.  All files in build_dir are uploaded to
/sd/apps/<app_id>/ on the device SD card.

Requires: pip install pyserial
"""

import sys
import os
import struct
import zlib
import time
import argparse
from datetime import datetime

try:
    import serial
except ImportError:
    print("Error: pyserial is not installed.  Run: pip install pyserial", file=sys.stderr)
    sys.exit(1)

# ---- Protocol constants -----------------------------------------------------

BAUD_RATE     = 115200
SOF           = b'\x01JPP'
PROTO_VERSION = 1
CHUNK_SIZE    = 1024

CMD_SESSION_START   = 0x00
CMD_SESSION_END     = 0x01
CMD_GET_INFO        = 0x02
CMD_SET_TIME        = 0x04
CMD_FS_MKDIR        = 0x11
CMD_FS_UPLOAD_BEGIN = 0x14
CMD_FS_UPLOAD_CHUNK = 0x15
CMD_FS_UPLOAD_END   = 0x16

ST_OK           = 0x00
ST_ERR_EXISTS   = 0x04

STATUS_NAMES = {
    0x00: 'OK',
    0x01: 'ERR_DENIED',
    0x02: 'ERR_NOT_FOUND',
    0x03: 'ERR_IO',
    0x04: 'ERR_EXISTS',
    0x05: 'ERR_INVALID',
    0x06: 'ERR_BUSY',
    0x07: 'ERR_NO_SESSION',
    0x08: 'ERR_TRANSFER',
    0x09: 'ERR_OVERFLOW',
    0x0A: 'ERR_APP_RUNNING',
}

# ---- CRC helpers ------------------------------------------------------------

def _crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, no reflection, no final XOR."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def _crc32(data: bytes) -> int:
    """CRC-32/ISO-HDLC — identical to Python zlib.crc32."""
    return zlib.crc32(data) & 0xFFFFFFFF

# ---- Frame builder ----------------------------------------------------------

def _build_frame(seq: int, cmd: int, body: bytes = b'') -> bytes:
    """Assemble a JPPD-SMP command frame."""
    payload   = bytes([seq, cmd, 0x00]) + body   # SEQ | CMD | FLAGS=0 | BODY
    plen      = len(payload)
    len_bytes = struct.pack('<H', plen)
    crc       = _crc16_ccitt_false(len_bytes + payload)
    return SOF + len_bytes + payload + struct.pack('<H', crc)

# ---- Frame reader -----------------------------------------------------------

def _read_frame(ser: serial.Serial, timeout: float) -> tuple:
    """
    Scan for the next valid response frame on *ser*.
    Returns (seq: int, status: int, body: bytes).
    Raises RuntimeError on timeout or CRC mismatch.
    """
    deadline  = time.monotonic() + timeout
    sof_idx   = 0

    # Scan byte-by-byte for the SOF magic.  ESP_LOG text output on the same
    # channel is transparently skipped because it never contains \x01JPP.
    # Fixed poll timeout, set once: reassigning ser.timeout on every single
    # byte read (as this used to) triggers a tcsetattr() on the underlying
    # tty each time, which is expensive enough on some USB-CDC drivers to eat
    # seconds of the deadline just scanning past ordinary log chatter.
    ser.timeout = 0.5
    while True:
        if time.monotonic() >= deadline:
            raise RuntimeError("Timeout waiting for response SOF")
        b = ser.read(1)
        if not b:
            continue
        byte = b[0]
        if byte == SOF[sof_idx]:
            sof_idx += 1
            if sof_idx == 4:
                break
        elif byte == SOF[0]:
            sof_idx = 1
        else:
            sof_idx = 0

    # Read 2-byte LEN field.
    ser.timeout = 2.0
    len_bytes = ser.read(2)
    if len(len_bytes) < 2:
        raise RuntimeError("Timeout reading LEN field")
    plen = struct.unpack_from('<H', len_bytes)[0]
    if plen < 2:
        raise RuntimeError(f"Invalid payload length {plen}")

    # Read payload. Bounded by the caller's own deadline — this used to force
    # a 10s floor regardless of how much of that deadline was already spent
    # scanning for SOF, which could silently blow well past a caller-supplied
    # timeout (e.g. SESSION_END's 5s).  A small floor just avoids a zero/negative
    # read() call right at the deadline edge.
    remaining = deadline - time.monotonic()
    ser.timeout = max(remaining, 0.5)
    payload = ser.read(plen)
    if len(payload) < plen:
        raise RuntimeError(f"Short payload: got {len(payload)}, expected {plen}")

    # Read 2-byte CRC.
    ser.timeout = 2.0
    crc_bytes = ser.read(2)
    if len(crc_bytes) < 2:
        raise RuntimeError("Timeout reading CRC field")
    recv_crc = struct.unpack_from('<H', crc_bytes)[0]

    calc_crc = _crc16_ccitt_false(len_bytes + payload)
    if calc_crc != recv_crc:
        raise RuntimeError(
            f"CRC mismatch: calculated 0x{calc_crc:04X}, received 0x{recv_crc:04X}"
        )

    return payload[0], payload[1], payload[2:]  # seq, status, body

# ---- Session class ----------------------------------------------------------

class _SMPSession:
    def __init__(self, port: str):
        self._ser = serial.Serial(port, BAUD_RATE, timeout=2.0)
        self._seq = 0

    def close(self) -> None:
        self._ser.close()

    def _next_seq(self) -> int:
        s = self._seq
        self._seq = (self._seq + 1) & 0xFF
        return s

    def _cmd(self, cmd: int, body: bytes = b'', timeout: float = 10.0) -> tuple:
        """Send *cmd* with *body*, wait for the matching response.  Returns (status, body)."""
        seq   = self._next_seq()
        frame = _build_frame(seq, cmd, body)
        self._ser.write(frame)
        resp_seq, status, resp_body = _read_frame(self._ser, timeout=timeout)
        if resp_seq != seq:
            raise RuntimeError(f"SEQ mismatch: sent {seq}, received {resp_seq}")
        return status, resp_body

    def _require_ok(self, status: int, context: str, also_ok: tuple = ()) -> None:
        if status != ST_OK and status not in also_ok:
            name = STATUS_NAMES.get(status, f"0x{status:02X}")
            raise RuntimeError(f"{context}: {name}")

    # ---- public commands ----

    def session_start(self) -> None:
        print("Waiting for device consent (press Allow on the device)...")
        status, _ = self._cmd(CMD_SESSION_START, bytes([PROTO_VERSION]), timeout=60.0)
        self._require_ok(status, "SESSION_START")

    def session_end(self) -> None:
        status, _ = self._cmd(CMD_SESSION_END, timeout=5.0)
        self._require_ok(status, "SESSION_END")

    def get_info(self) -> dict:
        """Parse GET_INFO body:
        [fw_version:NUL][username:NUL][hwid:NUL][sd_total:8 LE][sd_used:8 LE][sd_free:8 LE][sd_label:NUL]
        """
        status, body = self._cmd(CMD_GET_INFO)
        self._require_ok(status, "GET_INFO")

        def _read_cstr(buf: bytes, offset: int) -> tuple:
            end = buf.index(b'\x00', offset)
            return buf[offset:end].decode('utf-8', errors='replace'), end + 1

        fw_version, off = _read_cstr(body, 0)
        username,   off = _read_cstr(body, off)
        hwid,       off = _read_cstr(body, off)
        sd_total, sd_used, sd_free = struct.unpack_from('<QQQ', body, off)
        off += 24
        sd_label, off = _read_cstr(body, off)

        return {
            'fw_version': fw_version,
            'username':   username,
            'hwid':       hwid,
            'sd_total':   sd_total,
            'sd_used':    sd_used,
            'sd_free':    sd_free,
            'sd_label':   sd_label,
        }

    def set_time(self, dt: datetime = None) -> None:
        """Set the device RTC (in-RAM state, and DS1307 hardware if attached)
        from *dt* (defaults to the host's current local time).

        Body: [year:2 LE u16][month:1][day:1][weekday:1][hour:1][minute:1][second:1]
        weekday uses C tm_wday convention (0=Sunday..6=Saturday); the firmware
        doesn't interpret its meaning beyond storing it.
        """
        dt = dt or datetime.now()
        weekday = (dt.weekday() + 1) % 7  # Python Mon=0 -> C Sun=0
        body = struct.pack('<HBBBBBB', dt.year, dt.month, dt.day, weekday,
                           dt.hour, dt.minute, dt.second)
        status, _ = self._cmd(CMD_SET_TIME, body)
        self._require_ok(status, "SET_TIME")

    def mkdir(self, path: str) -> None:
        body   = path.encode() + b'\x00'
        status, _ = self._cmd(CMD_FS_MKDIR, body)
        # ERR_EXISTS is fine — directory already there
        self._require_ok(status, f"FS_MKDIR {path!r}", also_ok=(ST_ERR_EXISTS,))

    def upload_file(self, local_path: str, remote_path: str) -> None:
        data       = open(local_path, 'rb').read()
        file_size  = len(data)
        file_crc32 = _crc32(data)
        n_chunks   = (file_size + CHUNK_SIZE - 1) // CHUNK_SIZE if file_size else 0

        # --- UPLOAD_BEGIN ---
        begin_body = struct.pack('<I', file_size) + remote_path.encode() + b'\x00'
        status, resp = self._cmd(CMD_FS_UPLOAD_BEGIN, begin_body)
        self._require_ok(status, f"UPLOAD_BEGIN {remote_path!r}")
        xfer_id = resp[0]

        # --- UPLOAD_CHUNK (stop-and-wait) ---
        for idx in range(n_chunks):
            chunk      = data[idx * CHUNK_SIZE:(idx + 1) * CHUNK_SIZE]
            chunk_body = bytes([xfer_id]) + struct.pack('<H', idx) + chunk
            status, _ = self._cmd(CMD_FS_UPLOAD_CHUNK, chunk_body, timeout=20.0)
            self._require_ok(status, f"UPLOAD_CHUNK {idx}")
            pct = (idx + 1) * 100 // n_chunks
            print(f"  chunk {idx + 1}/{n_chunks}  {pct}%", end='\r', flush=True)

        if n_chunks:
            print()

        # --- UPLOAD_END ---
        end_body = bytes([xfer_id]) + struct.pack('<I', file_crc32)
        status, _ = self._cmd(CMD_FS_UPLOAD_END, end_body)
        self._require_ok(status, f"UPLOAD_END {remote_path!r}")

# ---- Entry point ------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description='Upload app artifacts to a JPP device via JPPD-SMP.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python3 scripts/jppd_upload.py /dev/ttyUSB0 games\n"
            "  python3 scripts/jppd_upload.py /dev/cu.usbserial-0001 meetapp build/apps/meetapp\n"
        ),
    )
    parser.add_argument('port',      help='Serial port (e.g. /dev/ttyUSB0)')
    parser.add_argument('app_id',    help='App ID matching the manifest (e.g. games, meetapp)')
    parser.add_argument('build_dir', nargs='?',
                        help='Build output directory (default: build/apps/<app_id>)')
    args = parser.parse_args()

    build_dir  = args.build_dir or os.path.join('build', 'apps', args.app_id)
    remote_dir = f'/sd/apps/{args.app_id}'

    if not os.path.isdir(build_dir):
        parser.error(f"Build directory not found: {build_dir}")

    files = sorted(
        f for f in os.listdir(build_dir)
        if not f.startswith('.') and os.path.isfile(os.path.join(build_dir, f))
    )
    if not files:
        parser.error(f"No files found in {build_dir}")

    total_bytes = sum(os.path.getsize(os.path.join(build_dir, f)) for f in files)
    print(f"Target:  {remote_dir}/")
    print(f"Files:   {len(files)}  ({total_bytes:,} B total)")
    for f in files:
        print(f"  {f}  ({os.path.getsize(os.path.join(build_dir, f)):,} B)")
    print()

    session = _SMPSession(args.port)
    try:
        session.session_start()

        info = session.get_info()
        sd_used_mb  = info['sd_used']  / (1024 * 1024)
        sd_total_mb = info['sd_total'] / (1024 * 1024)
        print(f"Connected: JPPDOS {info['fw_version']}  ({info['hwid']})")
        print(f"SD card:   {info['sd_label'] or '(no label)'}  "
              f"{sd_used_mb:,.1f} / {sd_total_mb:,.1f} MB used\n")

        session.mkdir(remote_dir)

        for filename in files:
            local  = os.path.join(build_dir, filename)
            remote = f'{remote_dir}/{filename}'
            size   = os.path.getsize(local)
            print(f"Uploading {filename}  ({size:,} B)...")
            session.upload_file(local, remote)
            print(f"  done")

        session.session_end()
        print(f"\nAll files uploaded to {remote_dir}/")

    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        try:
            session.session_end()
        except Exception:
            pass
        sys.exit(1)
    except Exception as exc:
        print(f"\nError: {exc}", file=sys.stderr)
        try:
            session.session_end()
        except Exception:
            pass
        sys.exit(1)
    finally:
        session.close()


if __name__ == '__main__':
    main()
