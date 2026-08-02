# MicroPython app development

MicroPython is the easiest way to build apps for the J++Device. You write Python, compile it once with `mpy-cross`, and copy the resulting file to the SD card. No Docker, no C toolchain, no firmware build step.

---

## Prerequisites

!!! danger "The bytecode ABI must match exactly."
    The firmware checks the bytecode ABI version declared in the manifest's
    [`toolchain` block](../manifest.md#toolchain) against the `.mpy` you shipped.
    A file compiled by an `mpy-cross` emitting a different ABI fails that check
    and the app will not load.

What has to match is the **bytecode ABI (mpy v6.3)**, which is what the
firmware checks. Any `mpy-cross` that emits v6.3 will do.

PyPI has no `1.28.0` release — it jumps from `1.27.0.post2` to a
`1.28.0rc0.post2` prerelease — so pin the prerelease, which emits v6.3:

```bash
pip install mpy-cross==1.28.0rc0.post2
```

Confirm what it emits — this line is the one that matters:

```bash
mpy-cross --version
# MicroPython v1.28.0-preview on ...; mpy-cross emitting mpy v6.3
```

!!! info "Prefer an exact 1.28.0 build?"
    Build it from source instead — this is what the firmware's own Docker
    image and CI do, and it also emits v6.3:

    ```bash
    git clone --depth 1 --branch v1.28.0 https://github.com/micropython/micropython.git
    make -C micropython/mpy-cross
    ```

    The binary lands at `micropython/mpy-cross/build/mpy-cross`; put it on your
    `PATH`.

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

!!! danger "Background runs are headless and time-boxed."
    No UI, no key events, and no consent prompts — every permission request is
    denied outright, so only tier-1 capabilities the user granted in a previous
    *foreground* launch are usable. Do your work and return within the
    **30-second quota**; a task that overruns is killed by restarting the
    device.

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
jppsdk.KEY_BACK          # the user asked to go back
jppsdk.KEY_CENTER_LONG   # older name for KEY_BACK, same value
```

!!! info "`KEY_BACK`, not `KEY_CENTER_LONG`."
    The two are the same value, so existing code is unaffected — but the name
    matters. Which *physical* gesture means "back" is a user preference
    (Settings → Controls: hold, or double-click), and your app never sees which
    one it was. `KEY_BACK` says what happened; `KEY_CENTER_LONG` guesses how.
    `KEY_BACK` needs [`sdk_min: 2`](../sdk-changelog.md); `KEY_CENTER_LONG`
    works at every level.

---

## Error handling

The `jppsdk` module raises two exception types:

| Exception | When |
|-----------|------|
| `jppsdk.SdkError` | Any non-OK status from the SDK (invalid argument, I/O error, etc.) |
| `jppsdk.SdkPermissionError` | Capability not granted — a subclass of `SdkError` |

!!! warning "Handle permission errors gracefully."
    The user can deny any capability, at any time. Show a message and degrade —
    never let a denial crash the app.

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

!!! info "Alternative: the `jppd-app-sdk` Docker toolchain."
    The [`jppdos-apps`](https://github.com/jppteam/jppdos-apps) repository ships
    a build image that does all of this for you —
    `docker run --rm -v "$PWD:/app" jppd-app-sdk` — with the correct
    `mpy-cross` version baked in, plus manifest validation and optional device
    upload. Handy if you don't want to manage `mpy-cross` versions yourself.
    See that repo's `toolchain/README.md`.

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

It declares `"sdk_min": 2` for one reason: it uses `KEY_BACK`. Swap that for
`KEY_CENTER_LONG` and `"sdk_min": 1` would run it on every unit ever shipped.

**manifest.json:**

```json
{
  "schema_version": 2,
  "app_id": "weather",
  "name": "Weather",
  "version": "1.0.0",
  "sdk_min": 2,
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
            self._fetch()
        elif key == jppsdk.KEY_BACK:
            self.sdk.request_close()

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
            "OK=refresh  Back=exit",
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
- `KEY_BACK` is the device-wide "back" or "cancel" gesture. Honour it wherever it makes sense to exit or go up a level — and don't try to work out whether the user held or double-clicked, because that is deliberately hidden from you.
