# Contributing to JPPDOS

This guide covers the day-to-day contributor workflow and the release
checklists for maintainers.

This project is mostly vibecoded, but still requires human actions for tagging
new releases. Agents should take a look at `AGENTS.md` for complete JPPDOS
source code knowledge base.

---

## 1. Making a new app for JPPDOS
All apps, except for some first-party ones, should be hosted in
https://github.com/jppteam/jppdos-apps. Please refer to the [Making new apps](https://github.com/jppteam/jppdos-apps#making-new-apps)
section of the repo.

## 2. Making a fix or a new feature for JPPDOS

### Branch model

- `master` — stable. Tagged releases are cut from here.
- `develop` — integration branch. Pre-releases are cut from here.
- `feature/<name>` / `fix/<name>` — your work. Branch from `develop`, open a
  PR back into `develop`. Merged `feature/*` / `fix/*` branches are swept
  automatically after a full release (`cleanup-branches.yml`).

GitHub (`github.com/jppteam/jppdos`) is the primary host; `git.nova.tokyo`
(GitLab) is a read-only mirror — never push there.

### Build & flash

To build the firmware, you don't need a local ESP-IDF install. Use Docker
instead:

```bash
docker compose run --rm build idf.py set-target esp32c6
docker compose run --rm build idf.py build
```

`scripts/flash.sh` wraps `idf.py flash monitor`; `scripts/jpp_deploy.sh <port>`
flashes via esptool and can upload built apps over JPPD-SMP in one step.

### Tests

Run the host-side suite before every push — CI runs the same thing:

```bash
python3 -m pytest tests
```

It covers docs/contract structure, manifest validation, the keypad and HTTP
state machines (compiled from the real `jpp_core` sources), and the SDK ABI
pins. If you touch a pure `jpp_core` state machine, add a host harness for it
following the `tests/keypad_harness.py` / `tests/httpd_harness/` pattern.

### Documentation is part of the change (non-negotiable)

Every `AGENTS.md`, `README.md`, and everything under `docs/` **must** be updated
in the *same commit* as any code or behaviour it describes — SDK surface,
broker policy, capability tiers, manifest schema, settings schema, boot
behaviour, hardware mapping, wire protocol, release milestone markers. Stale
docs are treated as a defect, not a follow-up.

> [!IMPORTANT]
> If you don't vibecode your change, you don't need to update the whole
> agent-facing documentation. Just state in your PR that those need updating.

- `docs/` pages are MkDocs-first (`!!! note` admonitions, `/// tab | C` blocks).
  Do not revert those to plain Markdown. Validate with
  `mkdocs build --strict` (via the `jppd-docs` image) after touching docs; a
  new page needs a `nav:` entry in `mkdocs.yml`.
- A new page or a `JPP_SDK_VERSION` bump also requires updates to
  `docs/sdk-changelog.md` and the `sdk_min` table in `docs/manifest.md`.

### If you change the App SDK surface

1. **Append only.** New `jpp_sdk_context_t` fields go at the struct tail; new
   enumerators at the enum tail. `jpp_sdk_native_services_t` is frozen — new
   service callbacks go in `jpp_sdk_services_v2_t`. Deployed app binaries read
   these offsets directly.
2. **Three registrations for a new `jpp_sdk_*` function**, not one:
   `s_symtab` in `jpp_native_symtab.c`, the binding + globals-table entry in
   `jpp_mp_sdk_module.c`, and a `Q(name)` line in
   `components/micropython/qstrdefsport.h`. Miss one and it fails differently
   (launch `UNRESOLVED_SYM`, Python `AttributeError`, or a build error).
3. **Keep native and MicroPython parity** — `jpp_mp_sdk_module.c` mirrors every
   `jpp_sdk_*` call except the two documented structural exceptions.
4. **Bump `JPP_SDK_VERSION`** to *(last released level) + 1* — not master + 1.
   Level 2 shipped in v1.1 and is closed; **level 3 is currently open**, so
   further backward-compatible additions land in 3. Keep changelog entries
   list-shaped so parallel branches merge by keeping both. Mirror the constant
   and `ALLOWED_CAPABILITIES` into `tests/validate_manifests.py`
   (`tests/test_sdk_abi.py` enforces this).
5. Update `apps/testapp_native/` and `apps/testapp_mp/` in the same commit —
   they are the reference exercisers and live in-tree so a surface change
   breaks their build.

### Other repeat pitfalls

- Never bypass the service broker for file/network/keypad/RTC/storage access.
- Do not put firmware-layer code (SSD1306 calls, settings screen) in
  `jpp_core/` — it belongs in `main/`.
- Do not auto-grant capabilities in `apply_consent()`; the `prompt_permission()`
  dialog is intentional security UX.
- Use the shared helpers (`jpp_string_util`, `jpp_file_util`, `jpp_nvs_util`,
  `jpp_draw_util`) rather than duplicating them.
- `jpp_core` public APIs return `jpp_<module>_result_t` / `_status_t`, never
  `esp_err_t`. Headers use `#pragma once`.
- The 5th keypad button is **OK**, never CENTER/CTR — fix stale terminology on
  sight (except the deprecated SDK aliases and the generic `center_uv` field).

### Before opening the PR

- [ ] `python3 -m pytest tests` passes.
- [ ] `docker compose run --rm build idf.py build` succeeds.
- [ ] Docs updated in the same commits as the behaviour.
- [ ] A `CHANGELOG.md` entry added under `## Unreleased` (product-facing wording
      — what shipped, not an API diff).
- [ ] No attribution/trailer lines in commit messages.
- [ ] PR targets `develop`.

---

## 2. Before tagging a pre-release (maintainer)

Pre-releases are normally cut from `develop`. A tag containing `rc`/`alpha`/
`beta`, or any tag whose commit is reachable from `develop` (but not yet from
`master`), publishes with `--prerelease` and a branch/SHA banner.

- [ ] Target commit is on `develop` and CI is green on it.
- [ ] `python3 -m pytest tests` passes (the release workflow gates on it and a
      tagged build must not ship broken).
- [ ] `## Unreleased` in `CHANGELOG.md` is current and readable — a `develop`
      tag usually predates its own `## <tag>` section, so release notes fall
      back to `## Unreleased`.
- [ ] `JPP_SDK_VERSION` and its Python mirror agree; the open level has a
      section in `docs/sdk-changelog.md` and a row in `docs/manifest.md`.
- [ ] `mkdocs build --strict` passes if docs changed.
- [ ] `JPPDOS_VERSION` in `main/jpp_settings_screen.h` — **mismatch with the tag
      is only a warning for a pre-release** (it is routinely tagged before the
      header bump), so this is optional but preferred.
- [ ] The IDF image cache is warm — `cache-idf-image.yml` runs on pushes to
      `master`; a tag cut from `develop` still falls back to the `master`-scoped
      cache, so no action unless `IDF_IMAGE` in `release.yml` changed.
- [ ] Push the tag (`git push origin <tag>`). After publish, verify the GitHub
      Release is marked *Pre-release* and the assets
      (`jppdos-<tag>-esp32c6-merged.bin`, the split-image zip, the apps zip,
      `SHA256SUMS.txt`) attached.
- [ ] `workflow_dispatch` with the tag input re-runs a failed publish.

---

## 3. Before tagging a release (maintainer)

Full releases are cut from `master` (the workflow checks `master` reachability
first, so once `develop` is merged the tag is a real release).

- [ ] `develop` is merged into `master` and CI is green on the merge commit.
- [ ] `python3 -m pytest tests` passes.
- [ ] **`JPPDOS_VERSION` in `main/jpp_settings_screen.h` exactly equals the tag**
      — this is **fatal** for a stable release; Settings > About must not
      self-report a different version.
- [ ] `CHANGELOG.md` has a `## <tag>` section (e.g. `## v1.1 — 2026-07-29`);
      its body is lifted verbatim into the release notes. Move the
      `## Unreleased` content into it.
- [ ] SDK level: if this release closes the currently-open level, that is now
      frozen — confirm `docs/sdk-changelog.md` and the `sdk_min` table in
      `docs/manifest.md` describe it completely, and that any in-flight branch
      re-targets *(this level) + 1* by hand.
- [ ] All docs in sync with shipped behaviour; `mkdocs build --strict` passes.
      The `docs.yml` workflow publishes `docs/` to
      `https://jppdevice.by.m4l3vi.ch/sdk-docs/` on push to `master`;
      `site_url` in `mkdocs.yml` must match that location.
- [ ] Update the snapshot/milestone markers at the top of `AGENTS.md` and any
      release-milestone references in `docs/` if this changes RTM/feature-release
      state.
- [ ] Hardware-touching changes: verified on real hardware, not just Wokwi
      (ADC, RTC, power-loss, Wi-Fi, SD). Re-check the `heap_mon` boot line
      against the `ESP_IDF_CONTRACT.md` 64 KB free-heap floor if RAM usage
      moved.
- [ ] Manufacturing unaffected, or `scripts/` (`build_images.sh`,
      `prepare_device.py`, `lrv_manufacturing.py`, record layout in
      `jpp_lrv.c`) updated together.
- [ ] Push the tag. Confirm the Release is marked *Latest* (not pre-release),
      all assets present, and the J++Device Manager `> [!TIP]` banner is
      prepended to the notes.
- [ ] After publish, `cleanup-branches.yml` runs off the release — check its
      dry run / results if you expect merged `feature/*` branches to be swept.
