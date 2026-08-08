# Changelog

Notable changes to JPPDOS, the firmware for the J++Device. This is a
firmware/hardware product, not a library — entries describe what shipped in
each build, not an API diff.

## Unreleased

### Firmware updates

- **The device can now update its own firmware from GitHub Releases —
  Settings > Firmware Update.** It checks the project's releases, downloads
  the new image to the SD card, verifies it against the release's published
  checksum, and only then installs it. Two toggles: "Auto-check" (off by
  default) checks once after boot whenever Wi-Fi connects, but never
  downloads or installs without you confirming; "Pre-releases" opts into
  release-candidate builds instead of stable-only (pre-enabled automatically
  if the firmware you're running is itself a pre-release).

  **This device has one firmware slot, not two — read this before using it.**
  Most OTA-capable devices keep a spare copy of the old firmware and fall
  back to it automatically if an update goes wrong. This one does not: the
  update overwrites the firmware currently running, so if power is lost
  while it's writing, the device will not boot on its own and needs a USB
  reflash to recover. The installer verifies the download's checksum before
  touching anything, and double-checks what actually landed in flash
  afterward, but neither of those helps if the device loses power mid-write.
  Keep it plugged in or on a healthy battery for the ~30 seconds the
  "UPDATING JPPDOS — Do NOT power off" screen is up, same as you would for
  updating a router.

### Apps

- **Apps get more room: the workspace grew from 64 KB to 80 KB.** That is the
  single block a running app lives in — program code for a C app, or the
  garbage-collected heap for a MicroPython one. Apps that were bumping against
  the ceiling now have 16 KB more, and a hub-plus-modules app can keep a bigger
  hub resident. This costs the device 16 KB of general-purpose memory, so it is
  a trade rather than free headroom.

  Note it is a property of the firmware, not of the SDK level: an app built to
  fill 80 KB will not load on v1.1, and there is no way to say "needs 80 KB" in
  a manifest.

### File transfer & device verification

- **WebDAV transfers are faster and no longer fight the Wi-Fi radio for
  memory.** The WebDAV server and the Device Info verification server used to
  run on ESP-IDF's stock HTTP server, which takes its working memory — task
  stack, connection state, buffers — from the same pool the Wi-Fi driver draws
  packet buffers from. On a device with one small block of RAM shared by
  everything, a big file transfer could starve the radio and wedge the
  connection mid-copy. Both servers now run in the same workspace the device
  reserves for running apps, which is otherwise sitting idle while you are on
  the WebDAV screen. Nothing is taken from the Wi-Fi side any more, and the
  file buffer grew from 4 KB to 32 KB, so copies do far fewer SD-card and
  network round trips.

- **The servers behave like apps now: they run in front, not behind.** Backing
  out of the WebDAV screen stops the server instead of leaving it quietly
  serving your SD card. Consequently the device will not sleep while either
  server is up, and starting a server while an app is running (or vice versa)
  is now impossible rather than merely discouraged.

- WebDAV also gained proper `HEAD` support, which some file managers use to
  check a file before downloading it.

- **The "Open Certificate Page" link on the Device Info verification screen now
  points at a real, self-contained verification page**
  (`https://jppdevice.by.m4l3vi.ch/verify`) instead of a placeholder domain.
  The link now carries the certificate and its manufacturer signature, not
  just the response signature, so the page can check authenticity on its own
  — it never needs to reach the device over the network to verify it.

### Serial protocol (JPPD-SMP)

- **The device now tells a connected PC when a management session ends
  because you held OK, instead of leaving it to find out the hard way.**
  Holding OK to end a session already closed it on the device; a host tool
  previously only learned about that from its next command failing
  (`ERR_NO_SESSION`) or a 30-second timeout. The device now sends an
  unsolicited notification the moment the session closes, so a host tool that
  is idling — waiting on the user, updating a UI — finds out immediately.
  `scripts/jppd_upload.py` (and the manufacturing scripts built on it) already
  understand it. See the [serial protocol reference](docs/serial-protocol.md#device-initiated-events)
  for the wire format if you're writing your own host tooling.

- **A management session no longer has to end just because nobody sent
  anything for 30 seconds.** New no-op `KEEPALIVE` command: a host tool that
  is idling — waiting on a user prompt, redrawing a UI — can send it to reset
  the session's inactivity timer without doing any actual work. Any real
  command already reset that timer; this just gives a host something to send
  when it has nothing else to say. See
  [0x05 — KEEPALIVE](docs/serial-protocol.md#0x05--keepalive) in the serial
  protocol reference. `scripts/jppd_upload.py`'s `_SMPSession.keepalive()` is
  the reference implementation.

### Build & release

- Pushing a version tag now builds the firmware and publishes a GitHub Release
  with flashable images attached (`.github/workflows/release.yml`). Release
  notes are taken from this file's section for that version.

- Tags cut from `develop` publish as **pre-releases**, tags cut from `master`
  as full releases. The branch is worked out from which one contains the tagged
  commit, since a tag push carries no branch of its own; anything reachable
  from neither is treated as a pre-release. `JPPDOS_VERSION` must match the tag
  for a stable release and is only warned about for a pre-release.

- Fixed the documented way to install `mpy-cross` for a host-side MicroPython
  build (needed by the release build itself, since `testapp_mp` hard-fails
  without it). The docs and the build's own error message said
  `pip install mpy-cross==1.28.0`, but PyPI has never published that exact
  version — the command could not have worked for anyone. Both now point at
  the prerelease build that emits identical bytecode, or building from source
  the way the project's Docker image and CI already do.

- **The firmware now targets the flash chip these boards actually have.**
  JPPDOS had been built for a 2 MB flash size — a v1.0-RTM-era correction that
  itself turned out to be wrong — but the real chip on production units is
  4 MB. The build now targets 4 MB (`CONFIG_ESPTOOLPY_FLASHSIZE_4MB`), and
  `partitions.csv` restores the margin the 2 MB layout had traded away:
  `data_fs`/`runtime_fs` are back to their original 256 KB each, the
  `coredump` partition dropped during that squeeze is back too, and `factory`
  grows to 3.4 MB of app headroom (from 1.9 MB) — roughly 1.6 MB (48%) free
  right after the switch, up from single-digit percent free under the old
  layout. `-Os` stays on regardless: it's no longer required to fit, but a
  smaller, faster-flashing image has no downside. Because a production batch
  is not guaranteed to be one uniform flash size, `scripts/prepare_device.py`
  now checks each unit's actual flash size against the image before every
  flash and refuses on a mismatch, rather than risk writing a 4 MB partition
  table onto a genuine 2 MB unit.

### App SDK & platform

- **A documented SDK call that never actually worked now works.** `wrap_text`,
  the helper that splits a long string into display-width lines, has been listed
  in the App Developer Guide as a native-app call since v1.0-RTM — but it was
  missing from the table the firmware uses to hand functions to apps, so any
  native app that called it was refused at launch with `UNRESOLVED_SYM`. It now
  loads and runs. Nothing about the function itself changed; it simply became
  reachable. This is the same defect that affected the `confirm` dialog helper
  in v1.1.

  This raises the App SDK to level **3**. Apps that use `wrap_text` should
  declare `sdk_min: 3` — and note that until a firmware carrying level 3 is
  released, such an app will not load on any unit in the field. Every existing
  app is unaffected: the surface only grows, so anything built for level 1 or 2
  keeps running untouched.

- **MicroPython apps can now do almost everything a native app can.** Sixteen
  calls that used to be C-only are now reachable from `jppsdk` too: front-loading
  a permission prompt, the confirm dialog, text wrapping, the file picker,
  acting as a BLE peripheral (advertising plus the GATT-server read/write
  handlers), opening outbound TCP connections, and all seven hardware-backed
  crypto primitives. Nothing about the underlying functions changed — only
  their reachability from Python did, which is why it shares SDK level 3 with
  `wrap_text` above; a MicroPython app that needs one of these should also
  declare `sdk_min: 3`. The only capabilities that remain native-only are
  loading a second compiled binary as a module (MicroPython has `import`
  instead) and a firmware-internal input hook no app calls directly. Both test
  apps (`testapp_native`, `testapp_mp`) were extended to exercise every one of
  the sixteen so the two stay proof that the surfaces genuinely match.

- **The 5th keypad button is now called OK everywhere, not CENTER.** Every
  identifier, on-screen label, and doc that named the button — firmware,
  App SDK, and the App Developer Guide alike — now says OK, matching how it
  was already labeled on the board and referred to in prose. One function and
  its constants were renamed as part of this: `jpp_sdk_claim_center` /
  `jppsdk.claim_center` and the `JPP_SDK_KEY_CENTER*` / `JPP_SDK_CENTER_CLAIM_*`
  constants it used are now `jpp_sdk_claim_ok` / `jppsdk.claim_ok` /
  `JPP_SDK_KEY_OK*` / `JPP_SDK_OK_CLAIM_*` — **but the old names still work.**
  They're kept as deprecated aliases (same values, same underlying function),
  so nothing already built needs to change. New C source using an old name
  gets a compiler warning naming its replacement; new code should switch to
  the OK-named forms, but nothing is forced to. See the
  [SDK changelog](docs/sdk-changelog.md#the-5th-keypad-button-is-ok-not-center)
  for the full list of renamed symbols and their aliases.

### Fixes

- **A placeholder in a text input looked exactly like a value you'd already
  typed, which made it easy to submit it by mistake.** `input()`'s
  `placeholder` text now renders in `[brackets]`, so example/hint text is
  visually distinct from something the user actually entered. The first
  keystroke still clears it and starts a real value, unchanged.

- **A fullscreen app could get stuck showing a stale system dialog.** An app
  that repaints its whole screen every frame — like the MTProto client — and
  only switches to fullscreen once at startup used to stay stuck in the
  windowed system-UI layout after showing a dialog, confirm, list, input, or
  file-picker prompt: the prompt's text lingered at the top of the screen and
  the bottom rows stopped updating. The five modal helpers now restore
  fullscreen automatically when they return, so this can no longer happen
  without the app doing anything differently.

## v1.1 — 2026-07-29

The first feature release after RTM. The headline is **App SDK level 2**, which
adds secure web requests, outbound TCP, and hardware-accelerated crypto to what
apps can do — plus a device-wide choice of how the Back button works.

Existing apps are unaffected: the SDK surface only grows, so anything built for
level 1 keeps running untouched. Going the other way, an app that declares
`sdk_min: 2` will **not** load on a v1.0-RTM device, so leave `sdk_min` at `1`
unless you actually use the new calls.

### Highlights

- **Apps can make HTTPS requests, one website at a time.** A new `https.request`
  capability lets an app talk to modern web APIs with the server's certificate
  properly checked — the old `http.request` was cleartext only. Permission is
  asked per website rather than once for the whole internet: the first time an
  app contacts, say, `api.example.com`, the device names that host and asks. Say
  yes and it never asks about that host again; if the app later starts
  contacting somewhere else, it has to ask you afresh. Certificate checking
  cannot be switched off by an app.
- **Apps can open ordinary network connections and do real crypto.** Alongside
  HTTPS, apps can now dial out over plain TCP (`network.connect`) and call
  hardware-accelerated SHA-256/SHA-1, AES-256-IGE, and big-integer
  modular-exponentiation/RSA/Diffie-Hellman primitives. The crypto is provided
  by the firmware rather than bundled into each app because an app has only
  64 KB to live in — carrying its own AES and bignum implementations would eat
  most of that before it did anything useful. Together these are what make a
  chat-protocol client feasible on the device at all.
- **Choose how the Back button works.** `Settings > Controls` now offers a
  device-wide choice between holding OK and double-clicking it to go back.
  Hold stays the default and behaves exactly as before, including the instant
  OK on a short click. Picking Double-click trades a short delay on OK (the
  device has to wait and see whether a second click is coming) for a Back
  gesture that doesn't require holding a button down.
- **Apps can take OK over as a game button.** An app may now claim the
  hold and/or double-click gesture as its own input, in which case it becomes
  responsible for its own way out — useful for games where OK is a
  rapid-fire action button and a stray double-tap shouldn't drop you out to
  the launcher. Apps that don't claim anything keep receiving a single "back"
  event and never have to care which gesture the user picked.

### App SDK & platform

- **The SDK now has a version number that means something.** The firmware
  exports API level **2**, and an app's manifest declares the lowest level it
  needs via `sdk_min`. Previously that field was parsed but never checked, so an
  app needing a newer firmware than the one it was installed on would load and
  then fail in some unpredictable way; it is now enforced up front with a clear
  `SDK_TOO_OLD` rejection. The matching `sdk_max` field was removed outright —
  the surface only ever grows compatibly, so there was nothing for an upper
  bound to protect against, and it only invited apps to lock themselves out of
  firmware that would have run them fine.
- **Manifests can name an author.** A free-form `author` string is now
  documented in the schema and set on all first-party apps. The firmware ignores
  it — it exists for the humans reading the manifest.

### Apps

- **MeetApp: the meetup leader can attach a comment.** Whoever runs the exchange
  may add a short free-text note (up to 100 characters) after confirming who is
  in the round — "Berlin meetup, March" and so on. It is part of the signed
  document rather than a local annotation, so it is forwarded to every
  participant and every copy of the proof verifies against the same text.
- **Games: quitting no longer throws away a new high score.** A score was only
  saved on game-over, so stopping mid-run at a personal best discarded it.
  Selecting Quit from the pause menu now saves first.
- **More things for the dim clock to say.** Additional idle-screen lines.

### Fixes

- **Apps installed before this update could have misbehaved after it.** An
  earlier change on the SDK v2 branch moved a field inside a structure that
  separately-built apps read directly, which would have made already-installed
  apps read the wrong values with no error message at all. The layout is
  restored and a test now pins it, so the same mistake fails the build instead
  of reaching a device.
- **Saving app data could fail when overwriting existing data.** The key-value
  store replaced its file by renaming a temporary one over the old one, which is
  not reliable on the SD card's FAT filesystem. The stale file is now removed
  first.

### Repo, build & documentation

- **App development split out into its own repository.** The `jppd-app-sdk`
  Docker toolchain (`tools/app-sdk/`) moved to
  [`jppdos-apps`](https://github.com/jppteam/jppdos-apps), which also gains a
  JPPD-SMP deploy tool with serial-port autodiscovery and a multiselect app
  picker. That repo does not vendor this one: its image build clones the
  firmware itself, always at the current tip of `master` unless told otherwise,
  so there is no submodule pointer to keep moving by hand and no way for the
  SDK image to quietly lag the firmware it is built from. App developers need
  no firmware checkout at all. Nothing about the on-device app format, the SDK
  surface, or the manifest schema changed; `docs/` here remains the source of
  truth for all three.
- **The example apps stay with the firmware.** The two App SDK test apps
  (`apps/testapp_native`, `apps/testapp_mp`) briefly moved out with the
  toolchain and now live here again, alongside Games, DemoScene and MeetApp.
  Keeping them in the firmware tree means they are rebuilt against the SDK on
  every firmware build, so a showcase app cannot quietly fall behind the surface
  it is demonstrating. Going the other way, the `mtproto` client skeleton — an
  ordinary app rather than an SDK demonstration — now lives in `jppdos-apps`.

- **No more submodules anywhere in the build.** The MicroPython interpreter used
  to arrive as a git submodule, which meant a plain `git clone` produced a tree
  that would not build until you remembered `git submodule update --init
  --recursive` — and then cost 1.6 GB of checkout plus 2.2 GB of git metadata to
  deliver the ~3.4 MB of interpreter source the firmware actually compiles.
  `idf.py build` now fetches a pinned, checksum-verified source archive itself
  (8 MB, cached outside `build/`), so cloning and building is one step again.
  The pinned revision is unchanged, so the resulting firmware is identical.
- **The App Developer Guide is published as a website.** `docs/` is now rendered
  as a searchable static site at
  [jppdevice.by.m4l3vi.ch/sdk-docs](https://jppdevice.by.m4l3vi.ch/sdk-docs/),
  republished automatically whenever the docs change on `master`. The Markdown
  in `docs/` stays the single source of truth — the site adds navigation, theme,
  and search on top of it, and nothing was forked or duplicated to build it.
- **Fixed a half-finished app move that broke the build.** Relocating the
  `mtproto` skeleton to `jppdos-apps` removed its sources but left its build
  files behind, leaving a registered component pointing at files that no longer
  existed — enough to fail `idf.py build` at configure time and to fail the
  host test suite. The leftovers are gone.
- **Fixed the MicroPython fetch aborting the build.** The switch away from the
  submodule fetched the interpreter during CMake configure, but ESP-IDF resolves
  component dependencies in an earlier pass that re-runs each component's build
  file in a restricted script mode where the fetch machinery cannot run. Every
  `idf.py build` stopped there, before compiling anything. The fetch is now
  skipped in that pass, which only reads the dependency list and compiles
  nothing.

### Known limitations

Unchanged from v1.0-RTM: no OTA update path, no LVGL UI redesign (the shell is
still the line-based SSD1306 text UI), a single shared MicroPython VM, and a
single foreground app at a time.

## v1.0-RTM — 2026-07-18

First tagged release. This build is the RTM (release to manufacturing)
milestone: the firmware, App SDK, and manufacturing/provisioning tooling are
considered complete and consistent enough to publish the repository and start
building units.

### Highlights

- **Sandboxed app platform.** Apps run from the SD card in either MicroPython
  or native C, against a shared App SDK, with a lazy/per-use capability
  system (ungated ↔ Tier 1 persisted-grant ↔ Tier 2 per-launch prompt) gating
  everything privileged — HTTP, BLE, ESP-NOW, background execution, and full
  SD access.
- **Built-in apps.** Settings, WebDAV file transfer, two SDK reference/test
  apps (MicroPython and native), Games (a native hub with 9 dynamically
  loaded game modules), DemoScene (an oldskool megademo), and MeetApp (a
  BLE contact-exchange app with Ed25519 identity) ship as the reference
  corpus for what the platform can do.
- **First-boot onboarding.** A one-shot welcome flow sets the device's
  username and offers to connect to Wi-Fi, without blocking normal boot on
  later runs.
- **Background scheduler.** Apps can declare interval/cron background tasks
  that run headlessly while the device idles on the launcher, with quota
  enforcement and preemption on user launch.
- **Limited Run Verification (LRV).** A write-once, unencrypted identity
  record lives on an external AT24C32 EEPROM (bound to the RTC breakout,
  independent of the ESP32-C6's own flash), signed at manufacturing time and
  served from an on-device HTTP verification endpoint. No sticker password
  or unlock step — a provisioned unit is verifiable straight from boot.
- **One-command manufacturing flow.** `scripts/prepare_device.py` flashes the
  provisioning image, opens a single JPPD-SMP session to read the unit's
  eFuse MAC, sync its clock, write the LRV identity once, and upload every
  app, then flashes the production image and records the unit in
  `scripts/ledger.csv` — one command per unit.
- **JPPD-SMP.** A binary management protocol over the device's native
  USB-Serial-JTAG port (no separate UART bridge chip on this board) for
  host-side file management, device info, LRV retrieval, and time sync,
  coexisting with the text log stream over the same wire.

### App SDK & platform

- Exposed the onboard WS2812 LED (`led_set_color`/`led_off`, ungated) and
  ESP-NOW (`espnow_send`/`espnow_recv`, Tier-1 `esp_now` capability) to apps.
- Added `jpp_sdk_request_cap()` so an app can front-load a permission prompt
  instead of surprising the user mid-flow.
- Exposed the device username to the SDK (`device_status()`) and made
  MeetApp default its identity nickname to it instead of prompting.
- Launcher now shows an app's manifest-declared display name instead of its
  SD folder name.
- App launch failures (not just runtime crashes) now surface a dialog with
  the failure reason, reusing the existing crash-dialog plumbing.

### Hardware & reliability

- Made the DS1307 RTC and AT24C32 EEPROM both optional: each is probed at
  boot and the device runs normally (clock-less, no LRV identity) if either
  is absent, rather than assuming they're always fitted.
- Fixed battery percentage reading a flat 0% (and later, reading well below
  actual voltage) by adding proper ADC calibration and 8-sample averaging
  instead of a flat linear conversion.
- Fixed BLE GATT reads/writes silently truncating above ~20 bytes (MTU
  negotiation + long-write/long-read), and hardened multi-round BLE
  exchanges in `jpp_ble_native` against partial reads.
- Fixed missing native-app symbol table entries that crashed apps calling
  newer SDK functions (`is_dummy_mode`, `net_close_all`) at launch.
- Fixed the EEPROM driver's write-cycle wait rounding to zero ticks at the
  platform's 100 Hz FreeRTOS tick, which was silently skipping the ACK
  poll and corrupting multi-page writes.

### Documentation & repo hygiene

- Reconciled `HARDWARE_SUMMARY.md` and `ESP_IDF_CONTRACT.md` against current
  source: flash budget (2 MB, not 4 MB), console peripheral (USB-Serial-JTAG,
  not UART0), the EEPROM and onboard LED (previously undocumented), and the
  `esp_now` capability and several SDK functions missing from the contract's
  capability/surface lists.
- Bumped the firmware version string to `1.0-RTM` and stamped the release
  snapshot across the top-level docs.
- Dropped stale "sticker password" wording left over from the switch to
  unencrypted, password-less LRV identity storage.
- Fixed `tests/test_manifests.py`, which failed on a clean checkout because
  the manifest scanner had no way to exclude `apps/common` (shared
  BLE-messaging source, not an app package) from discovery.

### Known limitations (not in this release)

- No OTA update path.
- No LVGL-based UI redesign — the shell is the existing line-based SSD1306
  text UI.
- Single shared VM: exactly one MicroPython runtime, no per-app isolation
  beyond the sandbox model.
- Single foreground app: one app owns the screen/input at a time; background
  tasks run headlessly, not concurrently with a second foreground app.
