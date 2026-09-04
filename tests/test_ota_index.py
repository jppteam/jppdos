"""OTA index and signature behaviour of scripts/ota_publish.py.

What is pinned here is the part the device depends on and CI cannot re-check
for itself: the `latest` index keeps the channel that is *not* being published,
version ordering decides when a pre-release entry is superseded rather than
left advertising a downgrade, and a .sig is a fixed 64 raw bytes that verifies
against the key that produced it.

Skips when `cryptography` is absent, in the same spirit as the harness tests
skipping without a C compiler.
"""

import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "scripts" / "ota_publish.py"

pytest.importorskip("cryptography",
                    reason="scripts/ota_publish.py needs the cryptography package")

sys.path.insert(0, str(REPO / "scripts"))
import ota_publish as ota  # noqa: E402


# --- version ordering ------------------------------------------------------ #

@pytest.mark.parametrize("lower,higher", [
    ("1.1", "1.2"),
    ("1.2", "1.10"),
    ("1.2", "1.2.1"),
    # A pre-release sorts below the release it prefixes, but above the
    # release before it — the rule the supersede check below relies on.
    ("1.3-rc1", "1.3"),
    ("1.2.1", "1.3-rc1"),
    ("1.3-rc2", "1.3"),
    # Numeric, not lexicographic: "rc10" must not sort below "rc2".
    ("1.3-rc2", "1.3-rc10"),
    ("1.0-RTM", "1.0"),
])
def test_version_ordering(lower, higher):
    assert ota.version_key(lower) < ota.version_key(higher)


def test_version_key_ignores_a_leading_v():
    assert ota.version_key("v1.2") == ota.version_key("1.2")


def test_version_key_pads_trailing_zeros():
    assert ota.version_key("1.2") == ota.version_key("1.2.0")


# --- index parsing / rendering --------------------------------------------- #

INDEX = """\
jppdos-index 1
generated 2026-09-04T00:00:00Z

# a comment, and a blank line, both ignored

[stable]
version 1.2
app 1.2.bin
"""


def test_parse_index_reads_sections_and_skips_comments():
    header, sections = ota.parse_index(INDEX)
    assert dict(header)["jppdos-index"] == "1"
    assert dict(sections["stable"])["version"] == "1.2"
    assert dict(sections["stable"])["app"] == "1.2.bin"


def test_parse_index_keeps_the_whole_value_after_the_first_space():
    _, sections = ota.parse_index("[stable]\nnote hello there world\n")
    assert dict(sections["stable"])["note"] == "hello there world"


def test_render_index_round_trips():
    _, sections = ota.parse_index(INDEX)
    _, again = ota.parse_index(ota.render_index(sections))
    assert again == sections


def test_render_index_orders_stable_before_prerelease():
    text = ota.render_index({"prerelease": [("version", "1.3-rc1")],
                             "stable": [("version", "1.2")]})
    assert text.index("[stable]") < text.index("[prerelease]")


# --- index updates --------------------------------------------------------- #

@pytest.fixture
def images(tmp_path):
    """A pair of stand-in images; only their size and digest are recorded."""
    def make(version):
        app = tmp_path / f"{version}.bin"
        merged = tmp_path / f"{version}-merged.bin"
        app.write_bytes(b"app" + version.encode())
        merged.write_bytes(b"merged" + version.encode())
        return app, merged
    return make


def section_for(images, version, channel_date="2026-01-01T00:00:00Z"):
    app, merged = images(version)
    return ota.build_section(version=version, tag=version, commit="c0ffee",
                             date=channel_date, app_path=app, merged_path=merged)


def test_publishing_one_channel_keeps_the_other(images):
    text, _ = ota.update_index("", "stable", section_for(images, "1.2"))
    text, _ = ota.update_index(text, "prerelease", section_for(images, "1.3-rc1"))
    _, sections = ota.parse_index(text)
    assert dict(sections["stable"])["version"] == "1.2"
    assert dict(sections["prerelease"])["version"] == "1.3-rc1"


def test_stable_release_drops_the_prerelease_it_supersedes(images):
    text, _ = ota.update_index("", "prerelease", section_for(images, "1.3-rc1"))
    text, notes = ota.update_index(text, "stable", section_for(images, "1.3"))
    _, sections = ota.parse_index(text)
    assert "prerelease" not in sections
    assert notes and "1.3-rc1" in notes[0]


def test_a_hotfix_keeps_a_newer_prerelease(images):
    # 1.3-rc1 is a preview of a *later* release than the 1.2.1 hotfix, so a
    # device on the pre-release channel must keep being offered it.
    text, _ = ota.update_index("", "prerelease", section_for(images, "1.3-rc1"))
    text, notes = ota.update_index(text, "stable", section_for(images, "1.2.1"))
    _, sections = ota.parse_index(text)
    assert dict(sections["prerelease"])["version"] == "1.3-rc1"
    assert notes == []


def test_republishing_the_same_version_is_allowed(images):
    """The retry path: workflow_dispatch over an already-published tag."""
    text, _ = ota.update_index("", "stable", section_for(images, "1.2"))
    text, _ = ota.update_index(text, "stable", section_for(images, "1.2"))
    _, sections = ota.parse_index(text)
    assert dict(sections["stable"])["version"] == "1.2"


def test_older_version_is_refused_without_force(images):
    text, _ = ota.update_index("", "stable", section_for(images, "1.2"))
    with pytest.raises(SystemExit):
        ota.update_index(text, "stable", section_for(images, "1.1"))
    text2, _ = ota.update_index(text, "stable", section_for(images, "1.1"),
                                force=True)
    _, sections = ota.parse_index(text2)
    assert dict(sections["stable"])["version"] == "1.1"


def test_unknown_channel_is_refused(images):
    with pytest.raises(SystemExit):
        ota.update_index("", "nightly", section_for(images, "1.2"))


def test_section_records_size_and_digest(images, tmp_path):
    app, merged = images("1.2")
    section = dict(ota.build_section("1.2", "1.2", "c0ffee",
                                     "2026-01-01T00:00:00Z", app, merged))
    assert section["app"] == "1.2.bin"
    assert section["app_size"] == str(app.stat().st_size)
    assert section["app_sha256"] == ota.sha256_file(app)
    assert section["merged_sha256"] == ota.sha256_file(merged)


def test_index_values_are_single_line_and_ascii(images):
    """The device parses this with a fixed-size line buffer and no unescaping."""
    text, _ = ota.update_index("", "stable", section_for(images, "1.2"))
    for line in text.splitlines():
        assert line.isascii()
        assert len(line) < 128


# --- signatures ------------------------------------------------------------ #

@pytest.fixture(scope="module")
def keypair():
    from cryptography.hazmat.primitives.asymmetric import ec
    key = ec.generate_private_key(ec.SECP256R1())
    return key, key.public_key()


def test_signature_is_64_raw_bytes(keypair):
    key, _ = keypair
    assert len(ota.sign_bytes(key, b"payload")) == ota.SIG_LEN == 64


def test_signature_round_trips(keypair):
    key, pub = keypair
    assert ota.verify_bytes(pub, b"payload", ota.sign_bytes(key, b"payload"))


def test_tampered_payload_fails(keypair):
    key, pub = keypair
    assert not ota.verify_bytes(pub, b"payloae", ota.sign_bytes(key, b"payload"))


def test_tampered_signature_fails(keypair):
    key, pub = keypair
    sig = bytearray(ota.sign_bytes(key, b"payload"))
    sig[0] ^= 0x01
    assert not ota.verify_bytes(pub, b"payload", bytes(sig))


def test_wrong_length_signature_fails(keypair):
    key, pub = keypair
    sig = ota.sign_bytes(key, b"payload")
    assert not ota.verify_bytes(pub, b"payload", sig[:-1])


def test_public_key_is_64_raw_bytes(keypair):
    _, pub = keypair
    assert len(ota.public_key_raw(pub)) == 64


# --- the CLI the workflow actually calls ----------------------------------- #

def run(*args, **kw):
    return subprocess.run([sys.executable, str(TOOL), *args],
                          capture_output=True, text=True, **kw)


def test_cli_keygen_sign_verify_round_trip(tmp_path):
    assert run("keygen", "--out-dir", str(tmp_path)).returncode == 0
    seckey = tmp_path / "ota_seckey.pem"
    pubkey = tmp_path / "ota_pubkey.pem"

    blob = tmp_path / "1.2.bin"
    blob.write_bytes(b"\x00\x01\x02\x03" * 64)

    assert run("sign", "--key", str(seckey), str(blob)).returncode == 0
    assert (tmp_path / "1.2.bin.sig").stat().st_size == 64
    assert run("verify", "--pubkey", str(pubkey), str(blob)).returncode == 0
    # The workflow's post-signing self-check derives the public half itself.
    assert run("verify", "--key", str(seckey), str(blob)).returncode == 0

    blob.write_bytes(b"\xff" * 256)
    assert run("verify", "--pubkey", str(pubkey), str(blob)).returncode != 0


def test_cli_verify_reports_a_missing_sig(tmp_path):
    assert run("keygen", "--out-dir", str(tmp_path)).returncode == 0
    blob = tmp_path / "unsigned.bin"
    blob.write_bytes(b"x")
    result = run("verify", "--pubkey", str(tmp_path / "ota_pubkey.pem"), str(blob))
    assert result.returncode != 0
    assert "MISSING" in result.stderr


def test_cli_index_update_merges_the_existing_index(tmp_path):
    app = tmp_path / "1.2.bin"
    merged = tmp_path / "1.2-merged.bin"
    app.write_bytes(b"a")
    merged.write_bytes(b"m")
    index = tmp_path / "latest"
    index.write_text("jppdos-index 1\n\n[prerelease]\nversion 1.9-rc1\n")

    result = run("index-update", "--index", str(index), "--out", str(index),
                 "--channel", "stable", "--version", "1.2", "--tag", "1.2",
                 "--commit", "c0ffee", "--app", str(app), "--merged", str(merged))
    assert result.returncode == 0, result.stderr
    text = index.read_text()
    assert "version 1.2" in text
    assert "version 1.9-rc1" in text
