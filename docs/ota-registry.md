# OTA updates and the App Hub

The device updates itself from a plain **HTTP** bucket. There is no TLS: the
hardware is too weak for it, and a certificate store the size of the trust
bundle would be a poor use of flash on a device that only ever talks to one
origin. Instead **every object in the bucket is signed**, and the device
trusts the signature rather than the transport.

This page is the wire contract between three things: the workflow that
publishes firmware ([`ota.yml`](https://github.com/jppteam/jppdos/blob/master/.github/workflows/ota.yml)),
the workflow that publishes apps to the **App Hub** (in the sibling
`jppdos-apps` repository), and the firmware that reads both.

!!! warning "The public key cannot be rotated."

    The verification key is compiled into the firmware. If the signing key is
    ever replaced, every already-flashed device rejects the new index and can
    only be recovered by reflashing it over USB with
    [J++Device Manager](https://jppdevice.by.m4l3vi.ch/#/manager). Treat the
    private key as a permanent, single-use secret.

## Storage convention

```text
/jppdos/latest                     index: newest stable + newest pre-release
/jppdos/<version>.bin              app-partition image (installed over the air)
/jppdos/<version>-merged.bin       whole-flash image (USB, via J++Device Manager)

/jppdos-apps/index                 index: every app in the App Hub
/jppdos-apps/<app_id>/manifest.json
/jppdos-apps/<app_id>/<app_id>.bin
/jppdos-apps/<app_id>/…            whatever else the app package contains
```

`<version>` is the release tag with any leading `v` stripped — the same string
the firmware reports in **Settings › About**, so the device can compare the two
directly with no parsing.

Every object listed above has a detached signature beside it, named by
appending `.sig` to the **full** file name: `latest.sig`, `1.2.bin.sig`,
`manifest.json.sig`. An inline signature (the PGP style) is not an option here
because most of these files are binary.

Version-stamped images are never overwritten or deleted, so an older release
stays fetchable indefinitely. Only `latest` and `index` are rewritten.

## Signature format

| Property | Value |
|---|---|
| Algorithm | ECDSA over NIST P-256 (secp256r1) |
| Digest | SHA-256 |
| `.sig` contents | exactly 64 raw bytes: `r ‖ s`, each a 32-byte big-endian integer |
| Public key in firmware | 64 raw bytes: the affine point `X ‖ Y` |

The signature is deliberately **not** DER-encoded. A fixed 64-byte file means
the device reads a known length and hands the bytes straight to mbedTLS
without an ASN.1 parser in the path. The public key is stored the same way —
the SEC1 uncompressed encoding with its leading `0x04` tag removed.

Generate the keypair with the publishing tool, which prints the C array to
paste into the firmware:

```bash
python3 scripts/ota_publish.py keygen --out-dir scripts/
```

To check a downloaded file by hand:

```bash
python3 scripts/ota_publish.py verify --pubkey scripts/ota_pubkey.pem 1.2.bin
```

## Index format

Both indexes use the same line-oriented format. It is not JSON: the parser on
the device is a fixed-size line buffer and a `strcmp`, and nothing here needs
nesting, escaping, or a tokeniser.

```text
jppdos-index 1
generated 2026-09-04T12:34:56Z

[stable]
version 1.2
tag 1.2
commit 022580c19bf3a1f0d1c5b2e7a4c9d8e6f0123456
date 2026-09-03T21:00:00Z
app 1.2.bin
app_size 1856944
app_sha256 a6143805115834beb551f72ff778aed4270751cae6e6d158794bac4c9556ab12
merged 1.2-merged.bin
merged_size 1902592
merged_sha256 674688cc5451808861b8375e00836876723c11379ad0c6ea484378b7906e68e4

[prerelease]
version 1.3-rc1
…
```

Parsing rules, in full:

- Lines are LF-terminated ASCII, under 128 bytes.
- A blank line, or one whose first character is `#`, is skipped.
- `[name]` opens a section. Everything after it belongs to that section until
  the next one.
- Any other line is a **key**, one space, and the **rest of the line** as the
  value. The value is never quoted and never continues onto a second line.
- Lines before the first section are the file header.
- **Unknown keys and unknown sections must be ignored, not rejected.** This is
  the only forward-compatibility mechanism the format has: it is how a newer
  publisher can add a field without bricking update checks on older firmware.

### Firmware index keys

| Key | Meaning |
|---|---|
| `jppdos-index` | Header. Format version, currently `1`. A device that does not recognise the number must stop, not guess. |
| `generated` | Header. When the index was written, ISO 8601 UTC. |
| `version` | Version string, and the file-name stem of both images. |
| `tag` | Git tag it was cut from — identical to `version` unless the tag carries a `v` prefix. |
| `commit` | Full 40-character commit SHA. |
| `date` | Commit date of that tag, ISO 8601 UTC. |
| `app`, `app_size`, `app_sha256` | The app-partition image: name relative to `/jppdos/`, size in bytes, and its SHA-256 as lowercase hex. |
| `merged`, `merged_size`, `merged_sha256` | The same three for the whole-flash image. |

There are exactly two sections, `[stable]` and `[prerelease]`, and either may be
absent. A device on the stable channel reads only `[stable]`.

!!! info "When a pre-release entry disappears."

    `[prerelease]` is dropped as soon as a stable release catches up with it —
    publishing 1.3 removes a 1.3-rc1 entry, since a device would otherwise be
    offered a downgrade. A pre-release of a *later* version survives an earlier
    stable release, so shipping the 1.2.1 hotfix leaves 1.3-rc1 in place.

## Publishing

`ota.yml` is a reusable workflow called by `release.yml` as its final job, so a
tag push builds the firmware, publishes the GitHub Release, and then signs and
uploads the same bytes — the images in the bucket are the images on the release
page. Running `ota.yml` on its own (**Actions › ota › Run workflow**, with a
tag) re-publishes from that release's assets instead, which is how a failed
upload is retried.

The job **skips itself with a warning** when the configuration below is
missing, so a checkout without OTA credentials still cuts releases normally. A
manual run fails loudly instead.

| Setting | Kind | Default |
|---|---|---|
| `OTA_S3_BUCKET` | variable | falls back to `S3_BUCKET` (the docs bucket) |
| `OTA_S3_PREFIX` | variable | `jppdos` |
| `S3_ENDPOINT_URL` | variable | `https://storage.yandexcloud.net` |
| `AWS_REGION` | variable | `ru-central-1` |
| `OTA_S3_ACCESS_KEY_ID` | secret | falls back to `S3_ACCESS_KEY_ID` |
| `OTA_S3_SECRET_ACCESS_KEY` | secret | falls back to `S3_SECRET_ACCESS_KEY` |
| `OTA_SIGNING_KEY` | secret | — (the EC P-256 private key, PEM) |

The credentials need `s3:ListBucket` on the bucket plus `s3:GetObject` and
`s3:PutObject` under the prefix. **No delete permission** is required, and the
workflow never issues one.

## Notes for the firmware side

- Fetch `latest` and `latest.sig`, verify, *then* act on the contents. There is
  a brief window during a publish where the two can disagree; a failed
  verification is a retry, not a fault.
- The version-stamped images land in the bucket **before** `latest` is
  rewritten, so an entry in the index always refers to an object that exists.
- `app_sha256` is a corruption check for the download, not a security control —
  the signature is what establishes trust. Check both.
- A signature that fails to verify is the key-rotation case described at the
  top of this page: tell the user to reflash over USB rather than retrying
  forever.
