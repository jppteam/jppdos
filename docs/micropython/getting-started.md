# MicroPython app development

**Contents:** [Prerequisites](#prerequisites) · [App layout](#app-layout) · [App skeleton](#app-skeleton) · [Key constants](#key-constants) · [Error handling](#error-handling) · [Compiling](#compiling) · [Deploying](#deploying) · [Complete example](#complete-example-weather-display) · [Tips](#tips)

---

MicroPython is the easiest way to build apps for the J++Device. You write Python, compile it once with `mpy-cross`, and copy the resulting file to the SD card. No Docker, no C toolchain, no firmware build step.

---

## Prerequisites

Install `mpy-cross` version **1.28.0** exactly — the firmware checks the bytecode ABI version and will reject `.mpy` files compiled with a different version:

```bash
pip install mpy-cross==1.28.0
```

Confirm the version:

```bash
mpy-cross --version
# MicroPython v1.28.0 on ...
```

---

## App layout

An app is a directory under `/sd/apps/<app_id>/`:

```
/sd/apps/my_app/
├── manifest.json   ← app metadata and capabilities
└── main.mpy        ← compiled bytecode (you compile main.py → main.mpy)
```

You can also include additional data files in the directory — they are accessible through the scoped file API.

---

## App skeleton

Every MicroPython app must define a `create_app(sdk)` function at module level. The firmware calls it once at launch and uses the returned object for the rest of the app's lifetime.

```python
import jppsdk

def create_app(sdk):
    return MyApp(sdk)

class MyApp:
    def __init__(self, sdk):
        self.sdk = sdk

    def on_start(self):
        # Called once when the app launches. Set up your initial state here.
        self.sdk.set_frame(["My App", "", "Press OK to quit"])

    def on_idle(self):
        # Called approximately every 100 ms while the app is in the foreground.
        # Poll for key events and update the display here.
        key = self.sdk.poll_key()
        if key == jppsdk.KEY_CENTER:
            self.sdk.request_close()

    def on_stop(self):
        # Called once before the app is torn down. Clean up resources here.
        pass
```

### Lifecycle hooks

| Hook | When called | Typical use |
|------|-------------|-------------|
| `on_start()` | Once, at launch | Initialize state, draw the first frame |
| `on_idle()` | ~100 ms intervals | Poll keys, refresh the display, do background work |
| `on_stop()` | Once, before teardown | Flush writes, release any held resources |
| `handle_action(key)` | When a key event fires (keypad task) | Alternative to polling — receives the key constant directly |

You can implement either `on_idle` (polling) or `handle_action` (event-driven), or both. Most apps use `on_idle` and call `poll_key()` inside it.

### Background task entry point

If your app uses background tasks (see [Manifest reference](../manifest.md)), the firmware calls a module-level function instead of `create_app` during headless runs:

```python
def on_task(name):
    # `name` is the task name from manifest.json background.tasks
    if name == "sync":
        # do work, write results to scoped storage
        pass
```

Background runs have no UI, no key events, and no consent prompts. Only Tier-1 capabilities that the user previously granted are available. Do your work and return within the 30-second quota.

---

## Key constants

```python
import jppsdk

jppsdk.KEY_NONE          # no key / timeout
jppsdk.KEY_UP
jppsdk.KEY_DOWN
jppsdk.KEY_LEFT
jppsdk.KEY_RIGHT
jppsdk.KEY_CENTER        # d-pad center press
jppsdk.KEY_CENTER_LONG   # d-pad center long-press (= BACK)
```

---

## Error handling

The `jppsdk` module raises two exception types:

| Exception | When |
|-----------|------|
| `jppsdk.SdkError` | Any non-OK status from the SDK (invalid argument, I/O error, etc.) |
| `jppsdk.SdkPermissionError` | Capability not granted — a subclass of `SdkError` |

Handle permission errors gracefully: show the user a message and degrade, do not crash:

```python
def on_start(self):
    try:
        result = self.sdk.http_request("GET", "http://192.168.1.1/data")
        self.data = result["body"]
    except jppsdk.SdkPermissionError:
        self.sdk.dialog("HTTP permission is required.", title="Permission needed")
        self.sdk.request_close()
    except jppsdk.SdkError as e:
        self.sdk.dialog(str(e), title="Error")
```

---

## Compiling

Compile `main.py` to `main.mpy` targeting the ESP32-C6's RISC-V instruction set:

```bash
mpy-cross -march=rv32imc -O2 main.py
```

This produces `main.mpy` in the same directory. The `-march=rv32imc` flag is required — omitting it produces bytecode for a different architecture that the device will refuse to load.

If your app has helper modules, compile each one the same way:

```bash
mpy-cross -march=rv32imc -O2 utils.py     # → utils.mpy
mpy-cross -march=rv32imc -O2 net.py       # → net.mpy
mpy-cross -march=rv32imc -O2 main.py      # → main.mpy
```

Include all the `.mpy` files (not the `.py` sources) in the app directory.

---

## Deploying

Copy the compiled app directory to the SD card:

```bash
cp -r my_app/ /Volumes/SD/apps/my_app/   # macOS
```

The directory must contain at minimum `manifest.json` and the entry `.mpy` file. Eject the card, insert it in the device, and the app will appear in the launcher on the next boot or launcher return.

If you update the app while the device is running, return to the launcher first — the device rescans on every launcher entry.

---

## Complete example: weather display

This app fetches a weather summary from a local endpoint, saves it to the KV store, and displays it. It refreshes every 30 minutes via a background task.

**manifest.json:**

```json
{
  "schema_version": 2,
  "app_id": "weather",
  "name": "Weather",
  "version": "1.0.0",
  "sdk_min": 1,
  "sdk_max": 1,
  "app_type": "micropython",
  "entry": "main.mpy",
  "capabilities": ["http.request", "background.register"],
  "background": {
    "enabled": true,
    "tasks": [{ "name": "fetch", "interval_s": 1800 }]
  },
  "toolchain": {
    "runtime_version": "v1.28.0",
    "cross_version": "1.28.0",
    "bytecode_abi": 6
  }
}
```

**main.py:**

```python
import jppsdk

ENDPOINT = "http://192.168.1.100/weather"

def on_task(name):
    if name != "fetch":
        return
    try:
        result = jppsdk.http_request("GET", ENDPOINT)
        summary = result["body"][:64]
        jppsdk.kv_set("summary", summary)
        jppsdk.kv_set("updated", jppsdk.get_time())
    except jppsdk.SdkError:
        pass  # will retry next interval


def create_app(sdk):
    return WeatherApp(sdk)


class WeatherApp:
    def __init__(self, sdk):
        self.sdk = sdk
        self.summary = None

    def on_start(self):
        self._load()
        self._draw()

    def on_idle(self):
        key = self.sdk.poll_key()
        if key == jppsdk.KEY_CENTER:
            self.sdk.request_close()
        elif key == jppsdk.KEY_CENTER_LONG:
            self._fetch()

    def on_stop(self):
        pass

    def _load(self):
        self.summary = self.sdk.kv_get("summary") or "No data yet"
        self.updated = self.sdk.kv_get("updated") or "never"

    def _fetch(self):
        try:
            result = self.sdk.http_request("GET", ENDPOINT)
            self.summary = result["body"][:64]
            self.updated = self.sdk.get_time()
            self.sdk.kv_set("summary", self.summary)
            self.sdk.kv_set("updated", self.updated)
            self.sdk.buzzer_play(jppsdk.SOUND_SUCCESS)
        except jppsdk.SdkPermissionError:
            self.sdk.dialog("HTTP permission needed.", title="Error")
        except jppsdk.SdkError:
            self.sdk.buzzer_play(jppsdk.SOUND_FAILURE)
        self._draw()

    def _draw(self):
        self.sdk.set_frame([
            "Weather",
            self.summary,
            "",
            "Updated: " + self.updated,
            "",
            "OK=exit  hold=refresh",
        ])
```

Compile and deploy:

```bash
mpy-cross -march=rv32imc -O2 main.py
cp manifest.json main.mpy /Volumes/SD/apps/weather/
```

---

## Tips

- `set_frame` takes up to 7 text lines. Each line fits roughly 21 characters in the default font.
- `on_idle` is called ~10 times per second, but it is fine to skip drawing every tick — only call `set_frame` when the content actually changes.
- `poll_key()` returns `KEY_NONE` immediately if nothing is in the queue. `wait_key(timeout_ms)` blocks; use `0` to wait forever. Use `poll_key()` in `on_idle` and `wait_key()` in blocking modal loops.
- The KV store (`kv_get`/`kv_set`/`kv_delete`) is the simplest way to persist app state — no file handling needed.
- Call `request_close()` when you want the launcher to reclaim the screen. After `on_stop` returns, the app is fully torn down.
- Long-press center (`KEY_CENTER_LONG`) is the conventional "back" or "cancel" gesture across the whole device. Honor it in your app wherever it makes sense to exit or go up a level.
