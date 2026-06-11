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

provision
    Create an LRV payload for one device and output a JPPDOS backup JSON that
    can be applied via Settings > Backup settings > Restore from file.

    Required flags:
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
      --output <file>         Write backup JSON to this file (default: stdout)

LRV certificate format (signed by the manufacturer)
----------------------------------------------------
    serial=<n>
    run_size=<n>
    device_type=<0|1>
    hwid=<AA:BB:CC:DD:EE:FF>
    device_pubkey=<64 lowercase hex chars>

Encrypted blob format (what gets stored in NVS)
-----------------------------------------------
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

The blob is base64-encoded and embedded in a JPPDOS backup JSON as:
    {"jppdos_backup": 1, "nvs_lrv": {"lrv_enc": "<base64>"}}
"""

import argparse
import base64
import json
import struct
import sys
from pathlib import Path

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

def cmd_provision(args: argparse.Namespace) -> None:
    # Load or generate device keypair.
    if args.device_seckey:
        dev_sk = load_seckey(args.device_seckey)
        print(f"Loaded device secret key from {args.device_seckey}")
    else:
        dev_sk = nacl.signing.SigningKey.generate()
        print("Generated new device keypair.")

    dev_vk             = dev_sk.verify_key
    dev_pubkey         = bytes(dev_vk)
    dev_seckey_libsodium = seckey_to_libsodium_bytes(dev_sk)

    # Load manufacturer secret key (used only for signing the certificate).
    mfr_sk = load_seckey(args.mfr_seckey)

    # Build and sign the certificate.
    cert_text = build_cert(args.serial, args.run_size,
                           args.device_type, args.hwid, dev_pubkey)
    cert_signed = mfr_sk.sign(cert_text.encode("utf-8"))
    cert_sig    = cert_signed.signature  # 64 bytes detached Ed25519 signature

    print(f"\nLRV Certificate:\n{cert_text}")
    print(f"Certificate signature: {cert_sig.hex()}\n")

    # Serialise plaintext (run_size and device_type are in the
    # certificate text; they are not stored as separate blob fields).
    plaintext = serialise_plaintext(
        serial           = args.serial,
        pubkey           = dev_pubkey,
        seckey_libsodium = dev_seckey_libsodium,
        cert_sig         = cert_sig,
        hwid             = args.hwid,
        cert             = cert_text,
    )

    # Encrypt.
    encrypted_blob = encrypt_blob(plaintext, args.password)
    b64_enc        = base64.b64encode(encrypted_blob).decode("ascii")

    # Build JPPDOS backup JSON (LRV data only — no other settings touched).
    backup = {
        "jppdos_backup": 1,
        "nvs_lrv": {
            "lrv_enc": b64_enc,
        }
    }
    backup_json = json.dumps(backup, separators=(",", ":"))

    if args.output:
        Path(args.output).write_text(backup_json, encoding="utf-8")
        print(f"Backup JSON written to: {args.output}")
    else:
        print(backup_json)

    print(f"\nDevice public key: {dev_pubkey.hex()}")
    print(f"Pubkey display:    {dev_pubkey[:3].hex().upper()}-{dev_pubkey[-3:].hex().upper()}")
    print(f"LRV password:      {args.password}")
    print(f"\nPrint this password on the inner sticker of unit #{args.serial}.")


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

    # provision
    p_pv = sub.add_parser("provision", help="Provision a device LRV backup")
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
    p_pv.add_argument("--output",
                      help="Output backup JSON file path (default: stdout)")

    args = parser.parse_args()

    if args.command == "keygen":
        cmd_keygen(args)
    elif args.command == "provision":
        if args.serial < 1 or args.serial > 65535:
            parser.error("--serial must be between 1 and 65535")
        if args.run_size < 1 or args.run_size > 65535:
            parser.error("--run-size must be between 1 and 65535")
        cmd_provision(args)


if __name__ == "__main__":
    main()
