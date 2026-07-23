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

- **Nav** is defined in `mkdocs.yml`. Add a page there when you add a doc under
  `docs/`, or it won't appear in the sidebar.
- **`docs/sdk-expansion.md`** is a firmware-internal design doc and is excluded
  from the published site (`exclude_docs` in `mkdocs.yml`).
- **Anchors:** the docs' hand-written `#anchor` links target GitHub's slug
  algorithm, so `mkdocs.yml` uses a GitHub-compatible `toc.slugify`. Two classes
  of link can't be reconciled by config and land at the top of the page on the
  site (they still work, or are already broken, on GitHub):
  - duplicate-heading anchors (GitHub `-1`, Python-Markdown `_1`) — e.g.
    `#capabilities-1` in `manifest.md`;
  - links to sub-anchors that were never real headings (broken on GitHub too) —
    e.g. `#shared_read`, `#jpp_sdk_module_load` in `sdk-reference.md`.
  Fixing these means editing the source docs (which improves GitHub rendering
  too); it is intentionally not done automatically here.
- **Publishing** (GitHub Pages / hosting) is not wired up yet — this is
  build/preview only.
