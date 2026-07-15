# JPPDOS App Developer Guide

**Guides:** [MicroPython](micropython/getting-started.md) · [Native C](native/getting-started.md) · [Code modules](native/modules.md) · [Manifest reference](manifest.md) · [SDK reference](sdk-reference.md) · [Serial protocol](serial-protocol.md)

**Contents:** [Hardware](#the-jdevice-at-a-glance) · [App model](#app-model) · [MicroPython vs native C](#choosing-micropython-or-native-c) · [Quick examples](#quick-examples) · [Where to go next](#where-to-go-next)

---

This guide covers everything you need to write apps for the **J++Device** — from the sandbox model and permission system to a complete API reference for every SDK call.

You do **not** need to understand firmware internals, ESP-IDF, or embedded systems to write apps. You write code against the **App SDK**, package it with a `manifest.json`, and copy it to the SD card. The device handles the rest.

---

## The J++Device at a glance

| Component | Detail |
|-----------|--------|
| Processor | ESP32-C6, RISC-V, single-core |
| Screen | SSD1306 128×64 monochrome OLED |
| Input | 5-way directional pad (up/down/left/right + center) |
| Storage | microSD card (apps and data) + internal flash (device settings) |
| Connectivity | Bluetooth LE, Wi-Fi |
| Clock | DS1307 real-time clock (battery-backed, optional — falls back to NTP / unset) |

Apps live on the SD card under `/sd/apps/<app_id>/`. The launcher discovers them at boot (and when you return to it after running an app), lists them by name, and runs whichever one the user selects.

---

## App model

Every app — regardless of language — is a small directory containing a `manifest.json` and one entry file:

```
/sd/apps/my_app/
├── manifest.json   ← describes the app and declares capabilities
└── main.mpy        ← compiled Python bytecode (or a native .bin)
```

Apps run in a **sandbox**: they can only reach hardware and OS services through the App SDK, and sensitive operations require a **capability** the user must grant. Granting is lazy — the prompt appears the first time the app actually uses that capability, not at launch, and the user can deny it without crashing the app.

The security model has two layers:

- **Scoping** — file paths are automatically prefixed to the app's own directory; an app cannot see another app's files without explicit permission.
- **Capabilities** — operations like HTTP requests, BLE, or full SD card access must be declared in the manifest and approved by the user.

Most of the SDK needs no permission at all: drawing, input, the buzzer, the onboard LED, the key-value store, IPC, and device status all work without any declaration.

---

## Choosing MicroPython or native C

| | MicroPython | Native C |
|-|-------------|----------|
| Language | Python (compiled to `.mpy`) | C (compiled to ELF32 RISC-V `.bin`) |
| Entry point | `create_app(sdk)` + lifecycle hooks | `jpp_app_entry(ctx)` — owns its own loop |
| Build tool | `mpy-cross 1.28.0` | `idf.py` via Docker (ESP-IDF) |
| When to use | Most apps — fast iteration, easy to debug | Tight loops, pixel-level games, BLE timing, code modules |
| Code modules | Not available | Load a second `.bin` from the app's own folder |

Both types use the same underlying SDK functions, the same capability/permission model, and the same manifest format. Most developers should start with MicroPython.

---

## Quick examples

**MicroPython — minimal clock app:**

```python
import jppsdk

def create_app(sdk):
    return ClockApp(sdk)

class ClockApp:
    def __init__(self, sdk):
        self.sdk = sdk

    def on_start(self):
        pass

    def on_idle(self):
        t = self.sdk.get_time()
        self.sdk.set_frame([t])

    def on_stop(self):
        pass
```

**Native C — minimal app:**

```c
#include "jpp_sdk_bridge.h"

void jpp_app_entry(jpp_sdk_context_t *ctx)
{
    const char *lines[] = { "Hello, world!" };
    jpp_sdk_set_frame(ctx, lines, 1);

    jpp_sdk_key_event_t key;
    while (true) {
        jpp_sdk_wait_key(ctx, 0, &key);
        if (key == JPP_SDK_KEY_CENTER)
            break;
    }
    jpp_sdk_request_close(ctx);
}
```

---

## Where to go next

| Document | What it covers |
|----------|---------------|
| [MicroPython guide](micropython/getting-started.md) | Writing, compiling, and deploying MicroPython apps |
| [Native C guide](native/getting-started.md) | Building native C apps with the ESP-IDF toolchain |
| [Native code modules](native/modules.md) | Loading a second `.bin` from within a native app |
| [Manifest reference](manifest.md) | Every field in `manifest.json`, all capabilities |
| [SDK reference](sdk-reference.md) | Complete API documentation for every SDK call |
| [Serial protocol](serial-protocol.md) | JPPD-SMP binary protocol for host-side tools (file management, device info, LRV, time sync) |
