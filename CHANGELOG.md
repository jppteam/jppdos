# Changelog

Notable changes to JPPDOS, the firmware for the J++Device. This is a
firmware/hardware product, not a library — entries describe what shipped in
each build, not an API diff.

## Unreleased

- **Choose how the Back button works.** `Settings > Controls` now offers a
  device-wide choice between holding CENTER and double-clicking it to go back.
  Hold stays the default and behaves exactly as before, including the instant
  OK on a short click. Picking Double-click trades a short delay on OK (the
  device has to wait and see whether a second click is coming) for a Back
  gesture that doesn't require holding a button down.
- **Apps can take CENTER over as a game button.** An app may now claim the
  hold and/or double-click gesture as its own input, in which case it becomes
  responsible for its own way out — useful for games where CENTER is a
  rapid-fire action button and a stray double-tap shouldn't drop you out to
  the launcher. Apps that don't claim anything keep receiving a single "back"
  event and never have to care which gesture the user picked.

- **App development split out into its own repository.** The `jppd-app-sdk`
  Docker toolchain (`tools/app-sdk/`) moved to
  [`jppdos-apps`](https://github.com/jppteam/jppdos-apps), which also gains a
  JPPD-SMP deploy tool with serial-port autodiscovery and a multiselect app
  picker. That repo vendors this one as a submodule tracking `master`, purely
  to build the SDK image — app developers need no firmware checkout at all.
  Nothing about the on-device app format, the SDK surface, or the manifest
  schema changed; `docs/` here remains the source of truth for all three.
- **The example apps stay with the firmware.** The two App SDK test apps
  (`apps/testapp_native`, `apps/testapp_mp`) briefly moved out with the
  toolchain and now live here again, alongside Games, DemoScene and MeetApp.
  Keeping them in the firmware tree means they are rebuilt against the SDK on
  every firmware build, so a showcase app cannot quietly fall behind the surface
  it is demonstrating. Going the other way, the `mtproto` client skeleton — an
  ordinary app rather than an SDK demonstration — now lives in `jppdos-apps`.

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
