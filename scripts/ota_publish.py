#!/usr/bin/env python3
"""
JPPDOS OTA / App Hub publishing tool.

Signs release artefacts and maintains the plaintext index that a device reads
to discover updates.  Used by .github/workflows/ota.yml; also runnable by hand
for key generation, verification, and one-off re-signing.

Requires: cryptography (pip install cryptography)

Subcommands
-----------
keygen
    Generate the OTA signing keypair (ECDSA P-256) and write it to
    <out-dir>/ota_seckey.pem (PKCS#8) + <out-dir>/ota_pubkey.pem (SubjectPublicKeyInfo).
    Also prints the 64-byte raw public key as a C array, ready to paste into
    the firmware.  The SECRET key never belongs in this repository: put its PEM
    into the OTA_SIGNING_KEY repository secret and delete the local file.

pubkey-c
    Re-print the C array for an existing public key, without generating one.

sign FILE [FILE ...]
    Write FILE.sig next to each FILE.

verify FILE [FILE ...]
    Check each FILE against FILE.sig, using --pubkey or, as a post-signing
    self-check, the public half of --key.  Exits non-zero if any file fails.

index-update
    Insert (or replace) one channel's section in the `latest` index, deriving
    sizes and SHA-256 digests from the artefacts on disk.

Signature format
----------------
ECDSA over the NIST P-256 curve (secp256r1), SHA-256 message digest.  The .sig
file is exactly 64 raw bytes -- r || s, each a 32-byte big-endian integer.
Not DER: a fixed-size signature means the device reads 64 bytes and hands them
straight to mbedtls with no ASN.1 parser in the way.

The public key is hardcoded in the firmware as the 64-byte raw affine point
X || Y (i.e. the SEC1 uncompressed encoding minus its leading 0x04 tag).

Index format
------------
See docs/ota-registry.md.  Line-oriented and deliberately simpler than JSON:

    jppdos-index 1
    generated 2026-09-04T12:34:56Z

    [stable]
    version 1.2
    ...

Blank lines and lines starting with '#' are ignored; "[name]" opens a section;
every other line is a key, one space, and the rest of the line as the value.
"""

import argparse
import hashlib
import os
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, utils as asym_utils
    from cryptography.exceptions import InvalidSignature
except ImportError:  # pragma: no cover - exercised only without the dep
    print("Error: cryptography is required.  Install with: pip install cryptography",
          file=sys.stderr)
    raise SystemExit(1)


SIG_SUFFIX = ".sig"
SIG_LEN = 64
INDEX_MAGIC = "jppdos-index"
INDEX_FORMAT = 1
CHANNELS = ("stable", "prerelease")


# --------------------------------------------------------------------------
# keys and signatures
# --------------------------------------------------------------------------

def load_private_key(path=None, env=None):
    """Load the signing key from a PEM file or, preferably in CI, from an
    environment variable holding the PEM text."""
    if env:
        pem = os.environ.get(env)
        if not pem:
            raise SystemExit(f"Error: environment variable {env} is empty or unset.")
        data = pem.encode()
    elif path:
        data = Path(path).read_bytes()
    else:
        raise SystemExit("Error: one of --key or --key-env is required.")

    key = serialization.load_pem_private_key(data, password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey):
        raise SystemExit("Error: signing key is not an EC key.")
    if not isinstance(key.curve, ec.SECP256R1):
        raise SystemExit(f"Error: signing key uses {key.curve.name}, expected secp256r1.")
    return key


def load_public_key(path=None, env=None):
    if env:
        pem = os.environ.get(env)
        if not pem:
            raise SystemExit(f"Error: environment variable {env} is empty or unset.")
        data = pem.encode()
    elif path:
        data = Path(path).read_bytes()
    else:
        raise SystemExit("Error: one of --pubkey or --pubkey-env is required.")
    key = serialization.load_pem_public_key(data)
    if not isinstance(key, ec.EllipticCurvePublicKey):
        raise SystemExit("Error: public key is not an EC key.")
    return key


def public_key_raw(pub):
    """64 raw bytes: the SEC1 uncompressed point minus its 0x04 tag."""
    point = pub.public_bytes(serialization.Encoding.X962,
                             serialization.PublicFormat.UncompressedPoint)
    assert len(point) == 65 and point[0] == 0x04
    return point[1:]


def sign_bytes(key, data):
    """ECDSA-P256/SHA-256, returned as fixed-width r || s."""
    der = key.sign(data, ec.ECDSA(hashes.SHA256()))
    r, s = asym_utils.decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def verify_bytes(pub, data, sig):
    if len(sig) != SIG_LEN:
        return False
    r = int.from_bytes(sig[:32], "big")
    s = int.from_bytes(sig[32:], "big")
    try:
        pub.verify(asym_utils.encode_dss_signature(r, s), data,
                   ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def c_array(raw, name="JPP_OTA_PUBKEY"):
    lines = [f"/* ECDSA P-256 public key, raw X || Y (64 bytes). */",
             f"static const uint8_t {name}[64] = {{"]
    for i in range(0, len(raw), 8):
        chunk = ", ".join(f"0x{b:02x}" for b in raw[i:i + 8])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# version ordering
# --------------------------------------------------------------------------

def _suffix_parts(suffix):
    """Split a pre-release suffix into comparable 3-tuples.

    Digit runs sort before alphabetic ones and compare numerically, so
    "rc2" > "rc1" rather than the string comparison that would put "rc10"
    before "rc2".
    """
    parts = []
    for token in re.findall(r"\d+|[A-Za-z]+", suffix):
        if token.isdigit():
            parts.append((0, int(token), ""))
        else:
            parts.append((1, 0, token.lower()))
    return tuple(parts)


def version_key(version):
    """Sort key giving semver-ish ordering: 1.2.1 < 1.3-rc1 < 1.3 < 1.10.

    A trailing non-numeric suffix marks a pre-release, which sorts *below* the
    bare release it prefixes -- the same rule semver uses, and the reason a
    1.3-rc1 entry survives a 1.2.1 hotfix but not the 1.3 release itself.
    """
    v = version.strip()
    if v[:1] in ("v", "V"):
        v = v[1:]
    m = re.match(r"^(\d+(?:\.\d+)*)(.*)$", v)
    if not m:
        # Unparseable: order it below everything numeric, by text.
        return ((-1,), 0, ((1, 0, v.lower()),))
    nums = [int(x) for x in m.group(1).split(".")]
    nums += [0] * (4 - len(nums)) if len(nums) < 4 else []
    suffix = m.group(2).lstrip("-_.+")
    if not suffix:
        return (tuple(nums), 1, ())
    return (tuple(nums), 0, _suffix_parts(suffix))


# --------------------------------------------------------------------------
# index parsing / rendering
# --------------------------------------------------------------------------

def parse_index(text):
    """-> (header_pairs, {section: [(key, value), ...]}) preserving order.

    Unknown keys and unknown sections are kept verbatim, so a newer publisher
    adding a field cannot be silently dropped by an older one.
    """
    header = []
    sections = {}
    current = None
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1].strip()
            sections.setdefault(current, [])
            continue
        key, _, value = line.partition(" ")
        pair = (key, value.strip())
        if current is None:
            header.append(pair)
        else:
            sections[current].append(pair)
    return header, sections


def render_index(sections, generated=None):
    out = [f"{INDEX_MAGIC} {INDEX_FORMAT}",
           f"generated {generated or iso_now()}"]
    # Fixed channel order first, then anything else we did not recognise.
    names = [c for c in CHANNELS if c in sections]
    names += [n for n in sections if n not in CHANNELS]
    for name in names:
        pairs = sections[name]
        if not pairs:
            continue
        out.append("")
        out.append(f"[{name}]")
        out.extend(f"{k} {v}" for k, v in pairs)
    return "\n".join(out) + "\n"


def section_get(pairs, key, default=None):
    for k, v in pairs:
        if k == key:
            return v
    return default


def iso_now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def build_section(version, tag, commit, date, app_path, merged_path,
                  app_name=None, merged_name=None):
    app = Path(app_path)
    merged = Path(merged_path)
    return [
        ("version", version),
        ("tag", tag),
        ("commit", commit),
        ("date", date),
        ("app", app_name or app.name),
        ("app_size", str(app.stat().st_size)),
        ("app_sha256", sha256_file(app)),
        ("merged", merged_name or merged.name),
        ("merged_size", str(merged.stat().st_size)),
        ("merged_sha256", sha256_file(merged)),
    ]


def update_index(text, channel, section, force=False):
    """Replace one channel's section.  Returns (new_text, notes)."""
    if channel not in CHANNELS:
        raise SystemExit(f"Error: unknown channel {channel!r}.")
    _, sections = parse_index(text or "")
    notes = []

    new_version = section_get(section, "version")
    old_version = section_get(sections.get(channel, []), "version")
    if old_version is not None and not force:
        if version_key(new_version) < version_key(old_version):
            raise SystemExit(
                f"Error: refusing to demote the {channel} channel from "
                f"{old_version} to {new_version}.  Pass --force if this is "
                f"deliberate.")

    sections[channel] = section

    # A pre-release is a preview of some future release.  Once a stable one
    # ships, drop any pre-release it has caught up with -- otherwise a device
    # on the pre-release channel is offered a downgrade.  A pre-release of a
    # *later* version (1.3-rc1 while 1.2.1 ships as a hotfix) is kept.
    if channel == "stable" and "prerelease" in sections:
        pre_version = section_get(sections["prerelease"], "version")
        if pre_version is not None and version_key(pre_version) <= version_key(new_version):
            del sections["prerelease"]
            notes.append(f"dropped superseded pre-release {pre_version}")

    return render_index(sections), notes


# --------------------------------------------------------------------------
# subcommands
# --------------------------------------------------------------------------

def cmd_keygen(args):
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    sec_path = out_dir / "ota_seckey.pem"
    pub_path = out_dir / "ota_pubkey.pem"
    if sec_path.exists() and not args.force:
        raise SystemExit(f"Error: {sec_path} already exists.  Pass --force to overwrite.")

    key = ec.generate_private_key(ec.SECP256R1())
    sec_path.write_bytes(key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    sec_path.chmod(0o600)
    pub = key.public_key()
    pub_path.write_bytes(pub.public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo))

    print(f"Wrote {sec_path} (SECRET) and {pub_path}")
    print()
    print(c_array(public_key_raw(pub)))
    print()
    print("Next: store the contents of", sec_path, "in the OTA_SIGNING_KEY")
    print("repository secret, hardcode the array above in the firmware, then")
    print("DELETE the local secret key file.  It cannot be rotated without")
    print("reflashing every device over USB.")
    return 0


def cmd_pubkey_c(args):
    pub = load_public_key(args.pubkey, args.pubkey_env)
    print(c_array(public_key_raw(pub), args.name))
    return 0


def cmd_sign(args):
    key = load_private_key(args.key, args.key_env)
    for name in args.files:
        path = Path(name)
        sig = sign_bytes(key, path.read_bytes())
        sig_path = Path(str(path) + SIG_SUFFIX)
        sig_path.write_bytes(sig)
        print(f"signed {path} -> {sig_path}")
    return 0


def cmd_verify(args):
    # Verifying with the key that just signed is a self-check, not a trust
    # check -- but it is the one that catches a mangled secret or a truncated
    # write before the bucket fills up with objects no device can accept.
    if args.pubkey or args.pubkey_env:
        pub = load_public_key(args.pubkey, args.pubkey_env)
    elif args.key or args.key_env:
        pub = load_private_key(args.key, args.key_env).public_key()
    else:
        raise SystemExit("Error: one of --pubkey/--pubkey-env or --key/--key-env "
                         "is required.")
    failed = 0
    for name in args.files:
        path = Path(name)
        sig_path = Path(str(path) + SIG_SUFFIX)
        if not sig_path.exists():
            print(f"MISSING  {sig_path}", file=sys.stderr)
            failed += 1
            continue
        if verify_bytes(pub, path.read_bytes(), sig_path.read_bytes()):
            print(f"ok       {path}")
        else:
            print(f"BAD      {path}", file=sys.stderr)
            failed += 1
    return 1 if failed else 0


def cmd_index_update(args):
    existing = ""
    if args.index and Path(args.index).exists():
        existing = Path(args.index).read_text()

    section = build_section(
        version=args.version,
        tag=args.tag,
        commit=args.commit,
        date=args.date or iso_now(),
        app_path=args.app,
        merged_path=args.merged,
        app_name=args.app_name,
        merged_name=args.merged_name,
    )
    text, notes = update_index(existing, args.channel, section, force=args.force)
    Path(args.out).write_text(text)
    for note in notes:
        print(f"note: {note}")
    print(f"wrote {args.out} ({args.channel} = {args.version})")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="ota_publish.py",
        description="Sign JPPDOS OTA artefacts and maintain the release index.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("keygen", help="generate the OTA signing keypair")
    p.add_argument("--out-dir", default="scripts")
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_keygen)

    p = sub.add_parser("pubkey-c", help="print the public key as a C array")
    p.add_argument("--pubkey")
    p.add_argument("--pubkey-env")
    p.add_argument("--name", default="JPP_OTA_PUBKEY")
    p.set_defaults(func=cmd_pubkey_c)

    p = sub.add_parser("sign", help="write FILE.sig beside each FILE")
    p.add_argument("--key", help="PEM file holding the EC private key")
    p.add_argument("--key-env", help="env var holding the private key PEM")
    p.add_argument("files", nargs="+")
    p.set_defaults(func=cmd_sign)

    p = sub.add_parser("verify", help="check each FILE against FILE.sig")
    p.add_argument("--pubkey")
    p.add_argument("--pubkey-env")
    p.add_argument("--key", help="private key PEM file; its public half is used")
    p.add_argument("--key-env", help="env var holding the private key PEM")
    p.add_argument("files", nargs="+")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("index-update", help="replace one channel in the index")
    p.add_argument("--index", help="existing index to merge into (may be absent)")
    p.add_argument("--out", required=True)
    p.add_argument("--channel", required=True, choices=CHANNELS)
    p.add_argument("--version", required=True)
    p.add_argument("--tag", required=True)
    p.add_argument("--commit", required=True)
    p.add_argument("--date", help="ISO 8601 UTC; defaults to now")
    p.add_argument("--app", required=True, help="path to the app-partition image")
    p.add_argument("--merged", required=True, help="path to the merged image")
    p.add_argument("--app-name", help="name to record (defaults to the basename)")
    p.add_argument("--merged-name")
    p.add_argument("--force", action="store_true",
                   help="allow publishing an older version than the one on record")
    p.set_defaults(func=cmd_index_update)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
