#!/usr/bin/env python3
"""
prepare_device.py — One-command provisioning + app deploy for a fresh J++Device.

Turns the whole fresh-unit flow into a single command:

    scripts/prepare_device.py --config scripts/mfg.toml

For each board it:
  1. flashes the PROVISIONING firmware image  (build-prov/)
  2. opens ONE JPPD-SMP session (the provisioning image auto-accepts the
     first session after boot, so this step runs unattended):
       - GET_INFO   -> reads the device's own eFuse MAC as the hwid
                       (no more manual `esptool.py chip_id` copy/paste)
       - SET_TIME   -> syncs the device RTC to the host's current time
       - PROVISION_LRV -> write-once-writes the raw LRV identity to the EEPROM
       - uploads every app to the SD card (survives the production reflash)
  3. flashes the PRODUCTION firmware image     (build/) — write path gone
  4. appends the unit to the ledger CSV

Serial numbers auto-increment from the ledger (override with --serial).

Build the two images first (once per firmware version):
    scripts/build_images.sh

Requires: pyserial, PyNaCl.  Python 3.11+ (tomllib).

Safety:
  --dry-run       do everything read-only: no flash, no write-once provision,
                  no upload. Still opens a session + GET_INFO to show the hwid.
  --skip-prov-flash / --skip-prod-flash / --no-apps  skip individual stages.
"""

import argparse
import csv
import json
import os
import subprocess
import sys
import time
import tomllib
from datetime import datetime, timezone
from pathlib import Path

# Reuse the JPPD-SMP client + app-upload helpers and the LRV blob builder.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from jppd_upload import (  # noqa: E402
    _SMPSession, upload_app_dir, list_app_files,
    ST_OK, ST_ERR_EXISTS, ST_ERR_INVALID, STATUS_NAMES,
)
import lrv_manufacturing as lrv  # noqa: E402

try:
    import serial.tools.list_ports as list_ports
except ImportError:
    print("Error: pyserial is required.  Install with: pip install pyserial",
          file=sys.stderr)
    sys.exit(1)

REPO_ROOT = Path(__file__).resolve().parent.parent

ESPRESSIF_VID = 0x303A


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

def load_config(path: Path) -> dict:
    with open(path, "rb") as f:
        cfg = tomllib.load(f)
    for section in ("mfg", "images", "apps"):
        cfg.setdefault(section, {})
    return cfg


def resolve(path_str: str) -> Path:
    """Resolve a config path relative to the repo root."""
    p = Path(path_str)
    return p if p.is_absolute() else (REPO_ROOT / p)


# ---------------------------------------------------------------------------
# Port autodetect
# ---------------------------------------------------------------------------

def autodetect_port() -> str:
    """Return the serial port of the single connected ESP32 (Espressif VID).

    Raises SystemExit with guidance if zero or several candidates are found.
    """
    candidates = [p for p in list_ports.comports() if (p.vid == ESPRESSIF_VID)]
    if not candidates:
        # Fall back to obvious usbmodem/usbserial devices.
        candidates = [
            p for p in list_ports.comports()
            if "usbmodem" in p.device or "usbserial" in p.device
            or "ttyUSB" in p.device or "ttyACM" in p.device
        ]
    if len(candidates) == 1:
        print(f"Auto-detected port: {candidates[0].device}")
        return candidates[0].device
    if not candidates:
        raise SystemExit("No serial device found. Pass --port <dev> explicitly.")
    listing = ", ".join(p.device for p in candidates)
    raise SystemExit(f"Multiple serial devices found ({listing}). "
                     f"Pass --port <dev> to choose one.")


# ---------------------------------------------------------------------------
# Ledger / serial
# ---------------------------------------------------------------------------

LEDGER_HEADER = ["serial", "hwid", "device_pubkey", "timestamp"]


def read_ledger_serials(ledger_path: Path) -> list:
    if not ledger_path.exists():
        return []
    serials = []
    with open(ledger_path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                serials.append(int(row["serial"]))
            except (KeyError, ValueError):
                continue
    return serials


def next_serial(ledger_path: Path, serial_start: int) -> int:
    serials = read_ledger_serials(ledger_path)
    return (max(serials) + 1) if serials else serial_start


def append_ledger(ledger_path: Path, serial: int, hwid: str,
                  pubkey_hex: str) -> None:
    ledger_path.parent.mkdir(parents=True, exist_ok=True)
    new_file = not ledger_path.exists()
    with open(ledger_path, "a", newline="") as f:
        w = csv.writer(f)
        if new_file:
            w.writerow(LEDGER_HEADER)
        w.writerow([serial, hwid, pubkey_hex,
                    datetime.now(timezone.utc).isoformat(timespec="seconds")])


# ---------------------------------------------------------------------------
# Flashing (esptool, driven by <build_dir>/flasher_args.json)
# ---------------------------------------------------------------------------

def flash_image(port: str, build_dir: Path) -> None:
    """Flash an image using the offsets/settings idf.py recorded for it."""
    fa_path = build_dir / "flasher_args.json"
    if not fa_path.exists():
        raise SystemExit(f"{fa_path} not found. Build it first "
                         f"(scripts/build_images.sh).")
    with open(fa_path) as f:
        fa = json.load(f)

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32c6", "-p", port, "-b", "460800",
        "--before", "default_reset", "--after", "hard_reset",
        "write_flash",
        *fa["write_flash_args"],
    ]
    for offset, relpath in fa["flash_files"].items():
        cmd += [offset, str(build_dir / relpath)]

    print(f"== Flashing {build_dir.name}/ to {port} ==")
    subprocess.run(cmd, check=True)


# ---------------------------------------------------------------------------
# App list resolution
# ---------------------------------------------------------------------------

def resolve_apps(cfg: dict, prod_dir: Path) -> list:
    """Return a list of (app_id, build_dir Path) to upload."""
    spec = cfg["apps"].get("upload", "all")
    apps_root = prod_dir / "apps"
    if spec == "all":
        if not apps_root.is_dir():
            return []
        ids = sorted(d.name for d in apps_root.iterdir() if d.is_dir())
    elif isinstance(spec, list):
        ids = spec
    else:
        raise SystemExit(f"[apps].upload must be \"all\" or a list, got {spec!r}")
    out = []
    for app_id in ids:
        d = apps_root / app_id
        if not d.is_dir() or not list_app_files(str(d)):
            print(f"  warning: app '{app_id}' has no build output at {d}, skipping")
            continue
        out.append((app_id, d))
    return out


# ---------------------------------------------------------------------------
# Main flow
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(
        description="One-command provision + app deploy for a fresh J++Device.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--config", required=True, help="Manufacturing TOML (see mfg.example.toml)")
    ap.add_argument("--port", default="auto", help="Serial port, or 'auto' (default)")
    ap.add_argument("--serial", type=int, help="Override the auto-incremented serial")
    ap.add_argument("--dry-run", action="store_true",
                    help="Read-only: no flash, no write-once provision, no upload")
    ap.add_argument("--skip-prov-flash", action="store_true",
                    help="Assume the provisioning image is already flashed")
    ap.add_argument("--skip-prod-flash", action="store_true",
                    help="Leave the provisioning image on the device (do not reflash prod)")
    ap.add_argument("--no-apps", action="store_true", help="Do not upload apps")
    args = ap.parse_args()

    cfg = load_config(resolve(args.config) if not Path(args.config).is_absolute()
                      else Path(args.config))
    mfg = cfg["mfg"]

    mfr_seckey_path = resolve(mfg["mfr_seckey"])
    ledger_path     = resolve(mfg.get("ledger", "scripts/ledger.csv"))
    prov_dir        = resolve(cfg["images"].get("prov_dir", "build-prov"))
    prod_dir        = resolve(cfg["images"].get("prod_dir", "build"))
    run_size        = int(mfg["run_size"])
    device_type     = int(mfg["device_type"])
    serial_start    = int(mfg.get("serial_start", 1))

    if not mfr_seckey_path.exists():
        raise SystemExit(f"Manufacturer secret key not found: {mfr_seckey_path}\n"
                         f"Generate it with: scripts/lrv_manufacturing.py keygen")

    mfr_sk = lrv.load_seckey(str(mfr_seckey_path))
    port   = autodetect_port() if args.port == "auto" else args.port

    serial = args.serial if args.serial is not None else next_serial(ledger_path, serial_start)

    apps = [] if args.no_apps else resolve_apps(cfg, prod_dir)

    print(f"\n=== Preparing unit  serial={serial}  run_size={run_size}  "
          f"device_type={device_type} ===")
    print(f"Port: {port}   Apps: {', '.join(a for a, _ in apps) or '(none)'}"
          f"{'   [DRY RUN]' if args.dry_run else ''}\n")

    # --- Stage 1: flash provisioning image -------------------------------
    if not args.skip_prov_flash and not args.dry_run:
        flash_image(port, prov_dir)
        print("Waiting for device to boot...")
        time.sleep(5)

    # --- Stage 2: one SMP session: hwid -> provision -> apps -------------
    session = _SMPSession(port)
    provisioned = False
    try:
        session.session_start()
        info = session.get_info()
        hwid = info["hwid"]
        print(f"Connected: JPPDOS {info['fw_version']}   hwid={hwid}")

        if args.dry_run:
            print(f"[dry-run] would SET_TIME to {datetime.now():%Y-%m-%d %H:%M:%S}")
        else:
            session.set_time()
            print(f"Set device time to {datetime.now():%Y-%m-%d %H:%M:%S}")

        built = lrv.make_identity_record(
            mfr_sk=mfr_sk, serial=serial, run_size=run_size,
            device_type=device_type, hwid=hwid,
        )
        pubkey_hex = built["device_pubkey"].hex()

        if args.dry_run:
            print(f"[dry-run] would PROVISION_LRV: {len(built['record'])} bytes, "
                  f"pubkey={pubkey_hex[:12]}…")
            print(f"[dry-run] would upload apps: "
                  f"{', '.join(a for a, _ in apps) or '(none)'}")
        else:
            status = session.provision_lrv(built["record"])
            if status == ST_OK:
                provisioned = True
                print(f"Provisioned LRV: serial={serial} pubkey={pubkey_hex[:12]}…")
            elif status == ST_ERR_EXISTS:
                print("NOTE: device already provisioned (write-once) — "
                      "keeping its existing identity, continuing with apps.")
            elif status == ST_ERR_INVALID:
                raise SystemExit(
                    "PROVISION_LRV rejected (ERR_INVALID): this device is NOT "
                    "running a provisioning firmware build. Flash build-prov/ "
                    "first (scripts/build_images.sh), or drop --skip-prov-flash.")
            else:
                name = STATUS_NAMES.get(status, f"0x{status:02X}")
                raise SystemExit(f"PROVISION_LRV failed: {name}")

            for app_id, d in apps:
                upload_app_dir(session, app_id, str(d))

        session.session_end()
    finally:
        session.close()

    # --- Stage 3: flash production image ---------------------------------
    if not args.skip_prod_flash and not args.dry_run:
        flash_image(port, prod_dir)

    # --- Stage 4: ledger --------------------------------------------------
    if args.dry_run:
        print("\n[dry-run] complete — nothing was written to the device or ledger.")
        return

    if provisioned:
        append_ledger(ledger_path, serial, hwid, pubkey_hex)

    disp = f"{built['device_pubkey'][:3].hex().upper()}-{built['device_pubkey'][-3:].hex().upper()}"
    print("\n================= UNIT READY =================")
    print(f"  serial : {serial}")
    print(f"  hwid   : {hwid}")
    print(f"  pubkey : {disp}")
    if provisioned:
        print(f"  ledger : {ledger_path}")
    print("=============================================")


if __name__ == "__main__":
    main()
