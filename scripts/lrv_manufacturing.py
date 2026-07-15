#!/usr/bin/env python3
"""
J++Device Limited Run Verification (LRV) manufacturing tool.

Requires: PyNaCl (pip install pynacl)

Subcommands
-----------
keygen
    Generate a manufacturer Ed25519 keypair and write it to two binary files.
    - mfr_pubkey.bin  (32 bytes, public key)
    - mfr_seckey.bin  (64 bytes, libsodium seed||pubkey format)

provision-device
    Create an LRV payload for one device and write it to the device's AT24C32
    EEPROM over JPPD-SMP (single-use, write-once). The device must be running a
    PROVISIONING firmware build (idf.py menuconfig -> JPPDOS -> Enable LRV EEPROM
    provisioning, i.e. CONFIG_JPP_LRV_PROVISIONING=y). After provisioning, flash
    the production firmware (provisioning disabled) so the write-path is gone.

    Required flags:
      --port <dev>          Serial port of the target device (e.g. /dev/cu.usbmodemXXXX)
      --mfr-seckey <file>   Manufacturer secret key file (64 bytes)
      --serial   <n>        Serial number (1-65535)
      --run-size <n>        Total run size (1-65535)
      --device-type <0|1>  0 = limited run, 1 = honorary
      --hwid <str>          eFuse MAC address of the target device
                            (read from device with: esptool.py chip_id)
                            format: "AA:BB:CC:DD:EE:FF"
      --password <str>      Password to be printed on the device's inner sticker

    Optional:
      --device-seckey <file>  Reuse an existing device secret key file instead
                              of generating a new one.

LRV certificate format (signed by the manufacturer)
----------------------------------------------------
    serial=<n>
    run_size=<n>
    device_type=<0|1>
    hwid=<AA:BB:CC:DD:EE:FF>
    device_pubkey=<64 lowercase hex chars>

Encrypted blob format (what gets stored on the AT24C32 EEPROM)
--------------------------------------------------------------
The plaintext is a packed binary structure:
    serial     2 B LE
    pubkey     32 B
    seckey     64 B  (seed||pubkey, libsodium format)
    cert_sig   64 B
    hwid       24 B  (null-padded)
    cert_len   2 B LE
    cert       cert_len B (including NUL terminator)

Note: run_size and device_type are encoded in the certificate text and are
NOT stored as separate binary fields in the blob. The manufacturer public key
is never stored on the device at all — verifiers hold it out-of-band.

The plaintext is encrypted with libsodium crypto_secretbox_easy:
    key  = BLAKE2b-256(password)
    blob = nonce(24) || ciphertext (plaintext + 16-byte Poly1305 MAC)

The raw blob is pushed to the device over the JPPD-SMP PROVISION_LRV command
(0x30), which the provisioning firmware write-once-writes to the EEPROM's
IDENTITY region. It is never restorable via a user backup.
"""

import argparse
import struct
import sys
from pathlib import Path

# Reuse the JPPD-SMP client from the app-upload tool (same scripts/ directory).
sys.path.insert(0, str(Path(__file__).resolve().parent))
from jppd_upload import _SMPSession, ST_OK, ST_ERR_EXISTS, STATUS_NAMES  # noqa: E402

try:
    import nacl.signing
    import nacl.secret
    import nacl.hash
    import nacl.utils
    import nacl.encoding
except ImportError:
    print("Error: PyNaCl is required.  Install with: pip install pynacl", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def load_seckey(path: str) -> nacl.signing.SigningKey:
    """Load a 64-byte libsodium secret key (seed||pubkey) from a file."""
    raw = Path(path).read_bytes()
    if len(raw) != 64:
        raise ValueError(f"Secret key file must be 64 bytes, got {len(raw)}")
    seed = raw[:32]
    return nacl.signing.SigningKey(seed)


def seckey_to_libsodium_bytes(sk: nacl.signing.SigningKey) -> bytes:
    """Return the 64-byte libsodium-style secret key: seed || pubkey."""
    seed   = bytes(sk)          # 32-byte seed
    pubkey = bytes(sk.verify_key)  # 32-byte public key
    return seed + pubkey


def derive_key(password: str) -> bytes:
    """Derive a 32-byte symmetric key from the password using BLAKE2b."""
    return nacl.hash.generichash(
        password.encode("utf-8"),
        digest_size=32,
        encoder=nacl.encoding.RawEncoder
    )


def build_cert(serial: int, run_size: int, device_type: int,
               hwid: str, pubkey_bytes: bytes) -> str:
    """Return the LRV certificate text (to be signed by the manufacturer)."""
    pubkey_hex = pubkey_bytes.hex()
    return (
        f"serial={serial}\n"
        f"run_size={run_size}\n"
        f"device_type={device_type}\n"
        f"hwid={hwid}\n"
        f"device_pubkey={pubkey_hex}\n"
    )


def serialise_plaintext(serial: int,
                         pubkey: bytes, seckey_libsodium: bytes,
                         cert_sig: bytes,
                         hwid: str, cert: str) -> bytes:
    """Pack all fields into the binary plaintext format expected by firmware.

    Layout:
        serial     2 B LE
        pubkey     32 B
        seckey     64 B
        cert_sig   64 B
        hwid       24 B (null-padded)
        cert_len   2 B LE
        cert       cert_len B (including NUL terminator)
    """
    cert_bytes = cert.encode("utf-8") + b"\x00"  # include NUL terminator
    cert_len   = len(cert_bytes)
    if cert_len > 191:
        raise ValueError("Certificate text too long (max 191 chars including NUL)")

    hwid_padded = hwid.encode("utf-8")[:23] + b"\x00" * (24 - len(hwid.encode("utf-8")[:23]))
    hwid_padded = hwid_padded[:24]

    header = struct.pack("<H", serial)
    return (header
            + pubkey           # 32 B
            + seckey_libsodium # 64 B
            + cert_sig         # 64 B
            + hwid_padded      # 24 B
            + struct.pack("<H", cert_len)
            + cert_bytes)


def encrypt_blob(plaintext: bytes, password: str) -> bytes:
    """Encrypt the plaintext with crypto_secretbox_easy (XSalsa20-Poly1305)."""
    key  = derive_key(password)
    box  = nacl.secret.SecretBox(key)
    # SecretBox.encrypt() prepends a random 24-byte nonce automatically.
    return bytes(box.encrypt(plaintext))


def make_encrypted_blob(mfr_sk: 'nacl.signing.SigningKey',
                        serial: int, run_size: int, device_type: int,
                        hwid: str, password: str,
                        device_sk: 'nacl.signing.SigningKey' = None) -> dict:
    """Build the full encrypted LRV blob for one device.

    Generates (or reuses) the device keypair, builds and signs the certificate
    with the manufacturer key, serialises + encrypts the plaintext. Pure data —
    no serial I/O — so both the CLI and the prepare_device orchestrator can use
    it. Returns a dict with keys: blob, device_pubkey, device_seckey, cert_text,
    cert_sig.
    """
    if device_sk is None:
        device_sk = nacl.signing.SigningKey.generate()
    dev_pubkey           = bytes(device_sk.verify_key)
    dev_seckey_libsodium = seckey_to_libsodium_bytes(device_sk)

    cert_text = build_cert(serial, run_size, device_type, hwid, dev_pubkey)
    cert_sig  = mfr_sk.sign(cert_text.encode("utf-8")).signature

    plaintext = serialise_plaintext(
        serial           = serial,
        pubkey           = dev_pubkey,
        seckey_libsodium = dev_seckey_libsodium,
        cert_sig         = cert_sig,
        hwid             = hwid,
        cert             = cert_text,
    )
    return {
        "blob":          encrypt_blob(plaintext, password),
        "device_pubkey": dev_pubkey,
        "device_seckey": dev_seckey_libsodium,
        "cert_text":     cert_text,
        "cert_sig":      cert_sig,
    }


# ---------------------------------------------------------------------------
# keygen
# ---------------------------------------------------------------------------

def cmd_keygen(args: argparse.Namespace) -> None:
    sk = nacl.signing.SigningKey.generate()
    vk = sk.verify_key

    pubkey_path = Path(args.output_dir) / "mfr_pubkey.bin"
    seckey_path = Path(args.output_dir) / "mfr_seckey.bin"

    pubkey_path.write_bytes(bytes(vk))
    seckey_path.write_bytes(seckey_to_libsodium_bytes(sk))

    print(f"Manufacturer keypair written:")
    print(f"  Public key : {pubkey_path}  ({bytes(vk).hex()})")
    print(f"  Secret key : {seckey_path}  (keep this safe!)")


# ---------------------------------------------------------------------------
# provision
# ---------------------------------------------------------------------------

CMD_PROVISION_LRV = 0x30


def cmd_provision_device(args: argparse.Namespace) -> None:
    # Load or generate device keypair.
    if args.device_seckey:
        dev_sk = load_seckey(args.device_seckey)
        print(f"Loaded device secret key from {args.device_seckey}")
    else:
        dev_sk = None
        print("Generating new device keypair.")

    # Load manufacturer secret key (used only for signing the certificate).
    mfr_sk = load_seckey(args.mfr_seckey)

    built = make_encrypted_blob(
        mfr_sk      = mfr_sk,
        serial      = args.serial,
        run_size    = args.run_size,
        device_type = args.device_type,
        hwid        = args.hwid,
        password    = args.password,
        device_sk   = dev_sk,
    )
    dev_pubkey     = built["device_pubkey"]
    encrypted_blob = built["blob"]

    print(f"\nLRV Certificate:\n{built['cert_text']}")
    print(f"Certificate signature: {built['cert_sig'].hex()}\n")
    print(f"Encrypted blob: {len(encrypted_blob)} bytes")

    # Push the blob to the device's EEPROM over JPPD-SMP (write-once).
    session = _SMPSession(args.port)
    try:
        session.session_start()
        status = session.provision_lrv(encrypted_blob)
        if status == ST_ERR_EXISTS:
            raise SystemExit(
                "Device already provisioned (write-once): its LRV IDENTITY "
                "region is already written and cannot be overwritten."
            )
        if status != ST_OK:
            name = STATUS_NAMES.get(status, f"0x{status:02X}")
            raise SystemExit(f"PROVISION_LRV failed: {name}")
        session.session_end()
    finally:
        session.close()

    print("\nProvisioned OK.")
    print(f"Device public key: {dev_pubkey.hex()}")
    print(f"Pubkey display:    {dev_pubkey[:3].hex().upper()}-{dev_pubkey[-3:].hex().upper()}")
    print(f"LRV password:      {args.password}")
    print(f"\nPrint this password on the inner sticker of unit #{args.serial}.")
    print("Now flash the PRODUCTION firmware (provisioning disabled).")


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="J++Device LRV manufacturing tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    # keygen
    p_kg = sub.add_parser("keygen", help="Generate manufacturer keypair")
    p_kg.add_argument("--output-dir", default=".",
                      help="Directory for key files (default: current dir)")

    # provision-device
    p_pv = sub.add_parser("provision-device",
                          help="Provision a device's EEPROM over JPPD-SMP")
    p_pv.add_argument("--port",         required=True,
                      help="Serial port of the target device (provisioning firmware)")
    p_pv.add_argument("--mfr-seckey",   required=True,
                      help="Manufacturer secret key file (64 bytes, from keygen)")
    p_pv.add_argument("--serial",       required=True, type=int,
                      help="Device serial number (1–65535)")
    p_pv.add_argument("--run-size",     required=True, type=int,
                      help="Total run size (embedded in certificate text)")
    p_pv.add_argument("--device-type",  required=True, type=int, choices=[0, 1],
                      help="0 = limited run, 1 = honorary (embedded in certificate text)")
    p_pv.add_argument("--hwid",         required=True,
                      help="eFuse MAC address (e.g. AA:BB:CC:DD:EE:FF). "
                           "Read from device with: esptool.py chip_id")
    p_pv.add_argument("--password",     required=True,
                      help="Password to print on inner sticker")
    p_pv.add_argument("--device-seckey",
                      help="Reuse existing device secret key file (64 bytes). "
                           "Omit to generate a fresh keypair.")

    args = parser.parse_args()

    if args.command == "keygen":
        cmd_keygen(args)
    elif args.command == "provision-device":
        if args.serial < 1 or args.serial > 65535:
            parser.error("--serial must be between 1 and 65535")
        if args.run_size < 1 or args.run_size > 65535:
            parser.error("--run-size must be between 1 and 65535")
        cmd_provision_device(args)


if __name__ == "__main__":
    main()
