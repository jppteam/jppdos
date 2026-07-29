# jppd-docs — docs site for the JPPDOS App Developer Guide

Renders the `docs/` tree as a searchable static site using **MkDocs** with the
**[mkdocs-shadcn](https://github.com/asiffer/mkdocs-shadcn)** theme. Python-only
— the theme ships prebuilt assets, so there is no Node in the pipeline.

`docs/` stays the single source of truth. The site adds only chrome (nav,
theme, search); it never forks content. Config lives in
[`mkdocs.yml`](../../mkdocs.yml) at the repo root.

## Preview locally (Docker, no host install)

```bash
# one-time: build the image (from the repository root)
docker build -f tools/docs/Dockerfile -t jppd-docs .

# live preview with auto-reload → http://localhost:8000
docker run --rm -p 8000:8000 -v "$PWD:/project" jppd-docs

# produce a static site into ./site/
docker run --rm -v "$PWD:/project" jppd-docs build
```

## Preview locally (host, no Docker)

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r tools/docs/requirements.txt
mkdocs serve        # http://localhost:8000
mkdocs build        # → ./site/
```

## Notes

- **The local preview lives under `/sdk-docs/`, not `/`.** `mkdocs serve` mounts
  the site at the path component of `site_url`, so open
  <http://localhost:8000/sdk-docs/>. A bare `http://localhost:8000/` 302s and
  every deep link under it 404s — that is configuration, not a broken build.
- **Nav** is defined in `mkdocs.yml`. Add a page there when you add a doc under
  `docs/`, or it won't appear in the sidebar.
- **`docs/sdk-expansion.md`** is a firmware-internal design doc and is excluded
  from the published site (`exclude_docs` in `mkdocs.yml`).
- **Check your links.** `mkdocs build --strict` reports every intra-doc anchor
  that doesn't resolve. Run it after touching `docs/`; it currently passes clean.
  Where a heading's own text would produce an awkward slug — anything suffixed
  `(C only)`, or a duplicate like `manifest.md`'s two `capabilities` headings —
  the anchor is pinned explicitly with `attr_list` (`### \`foo\` (C only) { #foo }`)
  rather than left to the slugifier.
- **The extension list is dictated by the theme, not by preference.** See the
  comment block above `markdown_extensions` in `mkdocs.yml` before changing it.
  In short: `codehilite` rather than `pymdownx.highlight`/`superfences`, because
  the theme's CSS targets the `.codehilite` wrapper class; and
  `pymdownx.blocks.tab` *without* `alternate_style`, because the theme styles
  only the legacy radio-input tab markup and ships no JS to drive the button
  variant. Swapping either produces a build that succeeds and a page that looks
  broken.
- **Admonition titles render inline with the first body paragraph** (a theme
  rule: `.admonition > p:first-child, .admonition > p:nth-child(2) { display: inline }`).
  End every `!!! type "Title"` with terminal punctuation, or the title runs into
  the body mid-sentence. Only `note`, `info`, `success`, `warning`, and `danger`
  have real styling; other types silently fall back to a plain box.
- **Publishing** is handled by [`.github/workflows/docs.yml`](../../.github/workflows/docs.yml):
  every push to `master` touching `docs/`, `mkdocs.yml`, or `tools/docs/` builds
  the site and syncs it to the `sdk-docs/` prefix of the docs bucket, serving
  <https://jppdevice.by.m4l3vi.ch/sdk-docs/>. Pull requests build but do not
  publish. Configure it once in the repository settings:

  | Kind | Name | Notes |
  |---|---|---|
  | Variable | `S3_BUCKET` | **Required** — the bucket name. The job fails early if unset. |
  | Variable | `S3_ENDPOINT_URL` | Defaults to `https://storage.yandexcloud.net`. |
  | Variable | `AWS_REGION` | Defaults to `ru-central-1`. |
  | Secret | `S3_ACCESS_KEY_ID` | Needs `ListBucket` on the bucket, plus `PutObject`/`DeleteObject` under `sdk-docs/*`. |
  | Secret | `S3_SECRET_ACCESS_KEY` | |

  The credentials are handed to the AWS CLI as environment variables rather than
  via `aws-actions/configure-aws-credentials`, which unconditionally validates
  them against AWS STS — something a non-AWS S3 provider cannot satisfy.
- **`site_url`** in `mkdocs.yml` points at that published location. Page links
  are relative so the site works from any prefix, but `sitemap.xml` is generated
  empty without it. Update it if the site ever moves.
