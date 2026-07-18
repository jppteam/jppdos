# JPPDOS — firmware for the J++Device

*Firmware v1.0-RTM · 2026-07-16*

JPPDOS (J++Device Operating System) is the firmware that runs the **J++Device**:
a small ESP32-C6 handheld with a monochrome OLED screen, a directional pad,
a microSD slot, a real-time clock, and Bluetooth LE. It boots straight into a
launcher, runs sandboxed apps from the SD card, and manages storage, settings,
Wi-Fi, Bluetooth, and power on their behalf.

This README gets you from a fresh checkout to a flashed device. If you want to
**write apps** for the J++Device, start with the [App developer docs](docs/index.md)
instead — you don't need anything in this file to do that.

---

## 1. What the J++Device is

| Part | Detail |
| --- | --- |
| Processor | ESP32-C6 (RISC-V, Wi-Fi + Bluetooth LE) |
| Screen | SSD1306 128×64 monochrome OLED |
| Input | 5-way directional pad (resistor-ladder keypad) |
| Storage | microSD card (apps and shared data) + internal flash (settings) |
| Clock | on-board real-time clock |

Apps live on the SD card under `/sd/apps/<app_id>/`. The device finds them at
boot, lists them in the launcher, and runs each one in a sandbox that can only
reach the hardware through permissions the user grants.

---

## 2. Build and flash it

You do **not** need to install ESP-IDF, toolchains, or Python. Everything runs
inside a Docker container, so the only things you install on your computer are
Docker and (for flashing) a USB connection to the device.

### Before you start

1. Install **Docker Desktop** (macOS/Windows) or Docker Engine (Linux), and make
   sure it is running.
2. Get the source code and open a terminal in its folder.
3. For flashing: plug the J++Device into your computer with a USB cable.

### Build the firmware

From the project folder, run:

```bash
docker compose run --rm build idf.py build
```

The first run downloads the toolchain and takes a while; later builds are fast.
When it finishes you'll see `Project build complete` and a `build/jppdos_idf.bin`
file is produced.

### Flash it to the device

With the device plugged in:

```bash
docker compose run --rm build scripts/flash.sh
```

This writes the firmware and then opens a serial monitor so you can watch it
boot. (Press `Ctrl-]` to exit the monitor.)

> On Linux you may need to give Docker access to the USB serial port. If the
> script can't find the device, see [Troubleshooting](#5-troubleshooting).

### What you'll see

On first boot the device formats its internal storage, mounts the SD card, loads
its settings, and shows the launcher. If no SD card is present it boots into a
safe recovery mode with installed apps hidden.

---

## 3. Simulate in Wokwi

Wokwi runs the firmware in a browser without physical hardware. A Wokwi build
differs from a hardware build in two ways that require an extra flag:

- **Keypad shim** — Wokwi can't model a resistor-ladder keypad, so the firmware
  substitutes digital GPIO push-buttons.  That shim must **not** be compiled
  into hardware builds (wrong GPIOs), so it is off by default.
- **BLE disabled** — Wokwi's BLE controller emulation is incomplete and causes
  an interrupt watchdog crash at boot, so the NimBLE stack is skipped when the
  shim flag is set.

Both are controlled by `-DJPP_WOKWI_SIM=ON`.

### Build for the simulator

```bash
docker compose run --rm build idf.py -DJPP_WOKWI_SIM=ON build
```

The build always produces `build/merged.bin` (bootloader + partition table + app
merged into a single flat flash image).  `wokwi/wokwi.toml` already
points at that file, so you can open the project straight in the Wokwi VS Code
extension or at wokwi.com after building.

### Embed an app in the simulator image

To test a single app without an SD card:

```bash
docker compose run --rm build idf.py \
  -DJPP_WOKWI_EMBED_APP_PATH=/path/to/my_app build
```

This packs the app directory (needs `manifest.json` + MicroPython `.mpy` files)
into the data_fs SPIFFS partition inside `merged.bin`.  The app then appears in
the launcher at runtime just like an SD-card app.  `-DJPP_WOKWI_SIM=ON` is set
automatically when `JPP_WOKWI_EMBED_APP_PATH` is supplied, so you don't need
both flags.

> **Wokwi is a topology reference only.** Analog keypad, DS1307 RTC, SD card
> reliability, Bluetooth, and power-loss behaviour are not verified by the
> simulator.

---

## 4. Configure it

The J++Device is configured **from its own screen** — open the **Settings** app
in the launcher. Settings covers shutdown/reboot, Wi-Fi (scan and connect),
time (NTP, timezone), sleep timers (standby and power-off durations), sound
(buzzer volume and startup jingle), SD card status, backup and restore of saved
settings, factory reset, the user's name, and an About screen. You do not need
to edit any files by hand.

Under the hood, settings are stored on internal flash at `/data/settings.json`
and are self-healing: if the file is ever missing or corrupt, the device resets
it to sensible defaults on the next boot, so a bad write can't brick it.

---

## 5. Write apps for it

Apps are the point of the J++Device. An app is a small folder on the SD card
with a `manifest.json` and one entry file, written in either **MicroPython**
(recommended — just Python) or native C. Apps draw to the screen, read the
d-pad, store data, and use Bluetooth/Wi-Fi through the **App SDK**, which also
gives you ready-made `dialog`, `list`, and `input` prompts so you don't have to
build menus from scratch.

➡️ **Start here: [App developer docs](docs/index.md)** — platform overview, MicroPython and native C guides, manifest reference, and a full SDK reference for every call.

---

## 6. Troubleshooting

| Symptom | What to check |
| --- | --- |
| Build fails immediately | Make sure Docker is installed and running, and that you're in the project folder. |
| Flash can't find the device | Confirm the USB cable is a data cable, the device is plugged in, and (Linux) that Docker can access the serial port. |
| Blank screen after flashing | Check the OLED ribbon/wiring; the panel must be SSD1306-compatible. |
| D-pad presses misread | Check the resistor-ladder wiring against the pin map in [HARDWARE_SUMMARY.md](HARDWARE_SUMMARY.md); each button must produce its expected ADC level. |
| Apps don't appear | Confirm the card is inserted and each app has `/sd/apps/<app_id>/manifest.json`. The device hides installed apps in recovery mode. |

The serial monitor prints markers that explain what happened, including
`SYSTEM_READY`, `SD_MOUNT_FAILED`, `BOOT_MODE`, `APP_REJECTED`,
`ACCESS_DENIED`, and `APP_CRASH`.

---

## 7. Reference

For anyone wiring or simulating the hardware:

- **Pin map** and the full hardware profile: [HARDWARE_SUMMARY.md](HARDWARE_SUMMARY.md)
- **Simulator:** the Wokwi project under `wokwi/` loads `build/merged.bin`.
  Build with `-DJPP_WOKWI_SIM=ON` — see [§3](#3-simulate-in-wokwi).
- **Firmware internals:** the native implementation lives under
  `components/jpp_core/`. See [AGENTS.md](AGENTS.md) for the component map.
- **Release notes:** see [CHANGELOG.md](CHANGELOG.md) for what shipped in
  each tagged release.
