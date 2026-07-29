# Native C app development

Native apps give you the full performance of the ESP32-C6 with direct access to the App SDK surface. You write C, build it alongside the firmware using the standard ESP-IDF toolchain inside Docker, and deploy the resulting `.bin` to the SD card.

Native apps share the same manifest format, capability/permission model, and SDK functions as MicroPython apps — the difference is only in the entry point and build system.

---

## Prerequisites

- **Docker Desktop** (macOS/Windows) or Docker Engine (Linux), running
- The JPPDOS firmware repository cloned locally

You do not need to install ESP-IDF, a RISC-V toolchain, or Python on your host — everything runs inside the Docker container.

---

## How native apps work

A native app binary is an **ELF32 RISC-V shared object** (`ET_DYN`) compiled with position-independent code flags. The firmware's native loader (`jpp_native_loader_core`) maps it into a 64 KB static pool in BSS at launch, resolves all undefined symbols from the firmware's exported symbol table, and calls your entry point.

All firmware functions (`jpp_sdk_*`, `esp_log_write`, standard C library functions) are resolved at load time from the symbol table — you never link against a separate SDK library. This means:

- Your app binary is small — it contains only your own code
- The app and firmware must be built from the same source tree so symbol names match
- You cannot call any function not in the firmware's symbol table

!!! danger "`UNRESOLVED_SYM` at launch means exactly one thing."
    Your app called a function the running firmware does not export. There is no
    link-time check to catch this — the loader is the first thing that notices.
    Either the firmware is older than the SDK level your app needs (declare the
    right [`sdk_min`](../manifest.md#sdk_min) so the loader rejects it with a
    clear `SDK_TOO_OLD` instead), or the symbol was never added to the
    firmware's export table at all.

---

## App layout

```
/sd/apps/my_app/
├── manifest.json   ← app metadata; app_type must be "native"
└── my_app.bin      ← ELF32 RISC-V shared object
```

Additional data files (configs, assets, game modules) can live alongside the `.bin` and are accessible via the scoped file API.

---

## Entry point

Every native app must export exactly one function:

```c
#include "jpp_sdk_bridge.h"

void jpp_app_entry(jpp_sdk_context_t *ctx);
```

The loader resolves this by name from the ELF symbol table and calls it directly. `jpp_app_entry` **owns its own loop** — it runs synchronously and the app exits when the function returns. There are no lifecycle hooks like the MicroPython model; you poll keys yourself:

```c
void jpp_app_entry(jpp_sdk_context_t *ctx)
{
    const char *frame[] = { "Counter", "", "0" };
    jpp_sdk_set_frame(ctx, frame, 3);

    int count = 0;
    jpp_sdk_key_event_t key;

    while (true) {
        jpp_sdk_wait_key(ctx, 0, &key);

        if (key == JPP_SDK_KEY_BACK) {
            break;  /* exit the app */
        } else if (key == JPP_SDK_KEY_UP) {
            count++;
        } else if (key == JPP_SDK_KEY_DOWN) {
            count--;
        }

        char val[16];
        snprintf(val, sizeof(val), "%d", count);
        const char *lines[] = { "Counter", "", val };
        jpp_sdk_set_frame(ctx, lines, 3);
    }

    jpp_sdk_request_close(ctx);
}
```

### Background task entry point

If your app uses background tasks, export this additional function (optional — if absent, background scheduling is not available):

```c
void jpp_app_task_entry(jpp_sdk_context_t *ctx, const char *task_name);
```

The firmware calls this instead of `jpp_app_entry` during headless background runs.

!!! danger "Background runs are headless and time-boxed."
    No UI, no key events, and no consent prompts — every permission request is
    denied outright, so only tier-1 capabilities the user granted in a previous
    *foreground* launch are usable. Return within the **30-second quota**; a
    task that overruns is killed by restarting the device.

---

## Build system integration

### Quick start: the `jppd-app-sdk` toolchain (recommended)

If you just want to build **your** app — not the whole firmware — use the
self-contained Docker toolchain from the
[`jppdos-apps`](https://github.com/jppteam/jppdos-apps) repository. It bakes a
firmware build as an SDK sysroot, so you need no firmware checkout, no `idf.py`,
and no `CMakeLists.txt` — just a prebuilt `jppd-app-sdk` image:

```bash
# from your app source directory (just manifest.json + src/**/*.c):
docker run --rm -v "$PWD:/app" jppd-app-sdk                # → ./dist/<app_id>/
docker run --rm -v "$PWD:/app" --device /dev/ttyACM0 \
    jppd-app-sdk --upload /dev/ttyACM0                     # build + upload
```

The tool auto-detects native vs MicroPython from your manifest, compiles all
`src/**/*.c`, validates the manifest, and stages `<entry>.bin` + `manifest.json`.
Shared helpers, extra includes, and defines go in an optional `jppd-app.json`
(see that repo's `toolchain/README.md`). The image is **version-locked** to a
firmware release — rebuild it when you move to a new firmware version.

To deploy several apps at once, with serial-port autodiscovery, use the same
repo's `./deploy.py` instead of `--upload`.

### Adding your app to the firmware build

The alternative is to build your app *inside* the firmware tree (this is how the
in-repo example apps build, and what you want when developing the firmware
itself).

Create `apps/my_app/CMakeLists.txt` following the pattern from an existing app (e.g. `apps/testapp_native/CMakeLists.txt`, the smallest single-binary example). The custom target calls `build_shared.py`, which:

1. Reads `build/compile_commands.json` for the correct include paths
2. Recompiles your sources as a RISC-V shared library (ELF32 ET_DYN)
3. Places the `.bin` and `manifest.json` in `build/apps/my_app/`

After adding a new `apps/` directory, run `idf.py reconfigure` once before building — ESP-IDF caches the component list:

```bash
docker compose run --rm build idf.py reconfigure
docker compose run --rm build idf.py build
```

The firmware and all app binaries are built together in one pass. After a clean build, your deployable package is ready at `build/apps/my_app/`.

### Build flags

Your sources are compiled with these flags (handled by `build_shared.py`):

```
-fPIC -shared -mno-relax -nostartfiles
-march=rv32imc_zicsr_zifencei -mabi=ilp32
```

Do not hard-code these — they come from `compile_commands.json`. Do not use `-O0`; the SDK headers include inline functions that require optimization to eliminate dead branches.

### Key types and headers

The only SDK header you need for most apps:

```c
#include "jpp_sdk_bridge.h"
```

This pulls in the full SDK surface: all `jpp_sdk_*` functions, `jpp_sdk_context_t`, `jpp_sdk_key_event_t`, `jpp_sdk_status_t`, `jpp_buzzer_note_t`, `jpp_sdk_ble_scan_result_t`, and all constants.

---

## Key types

### `jpp_sdk_context_t *ctx`

An opaque pointer to the SDK context bound to your app instance. Pass it as the first argument to every `jpp_sdk_*` call. Do not store it in a global — the context is only valid for the lifetime of `jpp_app_entry`.

### `jpp_sdk_status_t`

Return type for most SDK calls:

| Constant | Meaning |
|----------|---------|
| `JPP_SDK_OK` | Success |
| `JPP_SDK_INVALID_ARGUMENT` | Bad parameter |
| `JPP_SDK_INVALID_STATE` | Called at wrong time (e.g. no active session) |
| `JPP_SDK_ACCESS_DENIED` | Capability not granted by the user |
| `JPP_SDK_TEXT_TRUNCATED` | Output fit but was cut short |
| `JPP_SDK_BROKER_ERROR` | Internal broker failure |
| `JPP_SDK_HANDLE_LIMIT` | Too many open file handles |
| `JPP_SDK_INVALID_HANDLE` | Handle is not open or was closed |
| `JPP_SDK_BLE_CONN_LIMIT` | Too many open BLE connections |
| `JPP_SDK_INVALID_BLE_CONN` | Connection handle is not valid |
| `JPP_SDK_NO_DATA` | Requested data does not exist (e.g. missing KV key) |

### `jpp_sdk_key_event_t`

| Constant | Meaning |
|----------|---------|
| `JPP_SDK_KEY_NONE` | No key / timeout |
| `JPP_SDK_KEY_UP` | Up |
| `JPP_SDK_KEY_DOWN` | Down |
| `JPP_SDK_KEY_LEFT` | Left |
| `JPP_SDK_KEY_RIGHT` | Right |
| `JPP_SDK_KEY_CENTER` | Center press |
| `JPP_SDK_KEY_BACK` | The user asked to go back |
| `JPP_SDK_KEY_CENTER_LONG` | Older name for `JPP_SDK_KEY_BACK`, same value |
| `JPP_SDK_KEY_CENTER_HOLD` | Raw CENTER hold — only if claimed |
| `JPP_SDK_KEY_CENTER_DOUBLE` | Raw CENTER double-click — only if claimed |

!!! info "Which gesture means “back” is not your app’s business."
    The user chooses hold or double-click in Settings → Controls, and the
    firmware translates it to `JPP_SDK_KEY_BACK` before you see it. Take a
    gesture over as your own input only with
    [`jpp_sdk_claim_center`](../sdk/app-control.md#claim_center) — and then your
    app owns its own way out. `JPP_SDK_KEY_BACK` and the claim API need
    [`sdk_min: 2`](../sdk-changelog.md).

---

## Deploying

After the build, copy the entire contents of `build/apps/my_app/` to the SD card:

```bash
cp -r build/apps/my_app/ /Volumes/SD/apps/my_app/
```

The directory must contain `manifest.json` and the `.bin`. Any additional data files your app expects should also be included.

---

## Complete example: BLE device scanner

This app scans for nearby BLE devices and displays the results. The user can scroll through the list and view RSSI.

`"sdk_min": 2` here is needed for `JPP_SDK_KEY_BACK` alone — `ble.scan` itself
has been available since level 1.

**manifest.json:**

```json
{
  "schema_version": 2,
  "app_id": "blescanner",
  "name": "BLE Scanner",
  "version": "1.0.0",
  "sdk_min": 2,
  "app_type": "native",
  "entry": "blescanner.bin",
  "capabilities": ["ble.scan"],
  "background": { "enabled": false }
}
```

**blescanner.c:**

```c
#include "jpp_sdk_bridge.h"
#include <stdio.h>
#include <string.h>

#define MAX_RESULTS 20

static void show_scanning(jpp_sdk_context_t *ctx)
{
    const char *lines[] = { "BLE Scanner", "", "Scanning...", "", "Please wait" };
    jpp_sdk_set_frame(ctx, lines, 5);
}

static void show_results(jpp_sdk_context_t *ctx,
                         jpp_sdk_ble_scan_result_t *results,
                         size_t count, int cursor)
{
    char row1[32], row2[32], row3[32], row4[32];

    if (count == 0) {
        const char *lines[] = { "BLE Scanner", "", "No devices found",
                                 "", "OK=scan again" };
        jpp_sdk_set_frame(ctx, lines, 5);
        return;
    }

    snprintf(row1, sizeof(row1), "%zu found  [%d/%zu]",
             count, cursor + 1, count);
    const char *name = results[cursor].name[0]
                       ? results[cursor].name : "(unnamed)";
    snprintf(row2, sizeof(row2), "%.20s", name);
    snprintf(row3, sizeof(row3), "%s", results[cursor].address);
    snprintf(row4, sizeof(row4), "RSSI: %d dBm", results[cursor].rssi);

    const char *lines[] = { "BLE Scanner", row1, row2, row3, row4,
                             "", "OK=scan again" };
    jpp_sdk_set_frame(ctx, lines, 7);
}

void jpp_app_entry(jpp_sdk_context_t *ctx)
{
    jpp_sdk_ble_scan_result_t results[MAX_RESULTS];
    jpp_broker_result_t broker;
    size_t count = 0;
    int cursor = 0;
    bool scanned = false;

    const char *intro[] = { "BLE Scanner", "", "Press OK to scan" };
    jpp_sdk_set_frame(ctx, intro, 3);

    jpp_sdk_key_event_t key;
    while (true) {
        jpp_sdk_wait_key(ctx, 0, &key);

        if (key == JPP_SDK_KEY_BACK) {
            break;
        } else if (key == JPP_SDK_KEY_CENTER) {
            show_scanning(ctx);
            jpp_sdk_status_t st = jpp_sdk_ble_scan(ctx, 3000,
                                                     results, MAX_RESULTS,
                                                     &count, &broker);
            if (st == JPP_SDK_ACCESS_DENIED) {
                const char *denied[] = { "BLE Scanner", "",
                    "BLE scan denied.", "", "Back=exit" };
                jpp_sdk_set_frame(ctx, denied, 5);
                scanned = false;
                continue;
            }
            cursor = 0;
            scanned = true;
            show_results(ctx, results, count, cursor);
        } else if (scanned && count > 0) {
            if (key == JPP_SDK_KEY_DOWN && cursor < (int)count - 1) {
                cursor++;
                show_results(ctx, results, count, cursor);
            } else if (key == JPP_SDK_KEY_UP && cursor > 0) {
                cursor--;
                show_results(ctx, results, count, cursor);
            }
        }
    }

    jpp_sdk_request_close(ctx);
}
```

---

## Tips

- Always call `jpp_sdk_request_close(ctx)` before returning from `jpp_app_entry`. The firmware calls close on your behalf if you forget, but explicit cleanup is cleaner.
- Use `jpp_sdk_wait_key(ctx, 0, &key)` (timeout 0 = wait forever) as the main loop driver when you have no time-sensitive work to do between key events.
- Use `jpp_sdk_wait_key(ctx, 100, &key)` with a short timeout when you need periodic updates (e.g. refreshing a clock display).
- Check the return value of every SDK call. `JPP_SDK_ACCESS_DENIED` is expected and normal — handle it gracefully rather than treating it as a fatal error.
- Honour `JPP_SDK_KEY_BACK` wherever it makes sense to exit or go up a level. Don't try to detect the physical gesture behind it unless you have deliberately claimed one.
- The 64 KB app pool holds your entire binary. A typical app is 5–15 KB; complex apps with many data structures may approach 50 KB. If you need more, structure your code as a hub + loaded modules (see [Code modules](modules.md)).
- `snprintf` is available from the standard C library via the symbol table. `malloc`/`free` work too, but prefer stack allocation — the device heap is shared with Wi-Fi and other firmware services.
