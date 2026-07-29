# Native code modules

Code modules let a native app **page additional ELF binaries into the app pool on demand**, then unload and replace them. This is how the Games app ships 9 games in one 64 KB pool — the resident hub (~13 KB) loads one game module (~2–4 KB) at a time, runs it, and unloads it when the player returns to the menu.

Modules are a native-only feature. MicroPython apps cannot load modules.

---

## When to use modules

Use modules when your app's total code exceeds what fits comfortably in the 64 KB pool alongside your hub, or when you want to ship optional features that are only loaded on demand. The hub keeps running the whole time — the module runs inside the hub's task with the hub's SDK context and capabilities.

If your app fits in the pool as a single binary, you do not need modules.

---

## How the pool is split

The 64 KB app pool is used as follows during a module session:

```
[  hub binary  |     free     |  module binary  ]
^              ^              ^                  ^
pool start     hub image end  watermark          pool end
```

The hub loads into the low end of the pool at launch. When you call `jpp_sdk_module_load`, the module ELF is loaded into the **tail** of the pool from the watermark upward. Only one module can be resident at a time — loading a second module requires unloading the first.

---

## Module entry point

A module is a `.mod.bin` ELF32 RISC-V shared object (same format as an app `.bin`) that exports exactly one function:

```c
void jpp_module_entry(jpp_sdk_context_t *ctx, void *api);
```

- `ctx` — the hub's SDK context, passed through unchanged
- `api` — an app-defined pointer to whatever the hub wants to give the module (function table, state struct, etc.)

The `api` pointer carries no firmware semantics — it is whatever the hub and the module agree on. The Games hub, for example, passes a `games_api_t` struct containing score callbacks, canvas helpers, and exit flags.

The module entry runs synchronously inside the hub's task. When `jpp_module_entry` returns, `jpp_sdk_module_run` returns to the hub.

---

## Module API

```c
#include "jpp_sdk_bridge.h"
```

### `jpp_sdk_module_load`

Load a module ELF from the app's scoped storage into the pool tail.

```c
jpp_sdk_status_t jpp_sdk_module_load(jpp_sdk_context_t *ctx,
                                      const char *relative_path,
                                      void **out_module);
```

| Parameter | Description |
|-----------|-------------|
| `ctx` | The hub's SDK context |
| `relative_path` | Path to the `.mod.bin` file, relative to `/sd/apps/<app_id>/` |
| `out_module` | Receives a handle to the loaded module |

**Returns:** `JPP_SDK_OK` on success. Fails if the pool tail does not have enough room for the module, if the ELF is malformed, or if `relative_path` refers to a file outside the app's scoped directory.

On failure, the hub keeps running normally — module load failure is not fatal.

---

### `jpp_sdk_module_run`

Call the module's `jpp_module_entry` function.

```c
jpp_sdk_status_t jpp_sdk_module_run(jpp_sdk_context_t *ctx,
                                     void *module,
                                     void *api);
```

| Parameter | Description |
|-----------|-------------|
| `ctx` | The hub's SDK context |
| `module` | Handle from `jpp_sdk_module_load` |
| `api` | Pointer passed as the second argument to `jpp_module_entry` |

**Returns:** `JPP_SDK_OK` when `jpp_module_entry` returns. `JPP_SDK_INVALID_HANDLE` if `module` is not a valid loaded module.

This call blocks until the module returns. The hub's event loop is paused while the module runs; the module takes over key input and the display.

---

### `jpp_sdk_module_unload`

Release the module and free the pool tail back to the watermark.

```c
jpp_sdk_status_t jpp_sdk_module_unload(jpp_sdk_context_t *ctx,
                                        void *module);
```

| Parameter | Description |
|-----------|-------------|
| `ctx` | The hub's SDK context |
| `module` | Handle from `jpp_sdk_module_load` |

**Returns:** `JPP_SDK_OK`. After this call, `module` is invalid.

A module that is still loaded when `jpp_app_entry` returns is automatically unloaded by the firmware. You do not need to manually unload on the exit path.

---

## Building module binaries

Modules are built by the same `build_shared.py` helper as app binaries, but as separate targets. See `apps/games/CMakeLists.txt` for the reference pattern — it builds `games.bin` (the hub) plus one `<name>.mod.bin` per game.

The module's CMake target sets the entry symbol to `jpp_module_entry` instead of the default `jpp_app_entry`. The binary format is otherwise identical.

Module binaries live in `build/apps/<app_id>/` alongside the hub. Copy the entire directory to the SD card:

```bash
cp -r build/apps/games/ /Volumes/SD/apps/games/
```

---

## Complete example: plugin system

This example shows a hub app that loads one of two modules based on user selection.

### Shared header: `plugin_api.h`

Both the hub and each module include this header to define the interface:

```c
#pragma once
#include "jpp_sdk_bridge.h"

typedef struct {
    jpp_sdk_context_t *ctx;
    bool              should_exit;
} plugin_api_t;
```

### Hub: `hub.c`

```c
#include "jpp_sdk_bridge.h"
#include "plugin_api.h"

void jpp_app_entry(jpp_sdk_context_t *ctx)
{
    const char *menu[] = { "Plugin Hub", "", "1: Hello", "2: Counter", "",
                            "hold=exit" };
    jpp_sdk_set_frame(ctx, menu, 6);

    jpp_sdk_key_event_t key;
    while (true) {
        jpp_sdk_wait_key(ctx, 0, &key);
        if (key == JPP_SDK_KEY_BACK) break;

        const char *mod_path = NULL;
        if (key == JPP_SDK_KEY_UP)   mod_path = "hello.mod.bin";
        if (key == JPP_SDK_KEY_DOWN) mod_path = "counter.mod.bin";
        if (!mod_path) continue;

        void *mod = NULL;
        if (jpp_sdk_module_load(ctx, mod_path, &mod) != JPP_SDK_OK) {
            const char *err[] = { "Plugin Hub", "", "Load failed" };
            jpp_sdk_set_frame(ctx, err, 3);
            continue;
        }

        plugin_api_t api = { .ctx = ctx, .should_exit = false };
        jpp_sdk_module_run(ctx, mod, &api);
        jpp_sdk_module_unload(ctx, mod);

        /* restore hub frame after module exits */
        jpp_sdk_set_frame(ctx, menu, 6);
    }

    jpp_sdk_request_close(ctx);
}
```

### Module: `hello.c`

```c
#include "jpp_sdk_bridge.h"
#include "plugin_api.h"

void jpp_module_entry(jpp_sdk_context_t *ctx, void *api_ptr)
{
    plugin_api_t *api = (plugin_api_t *)api_ptr;
    const char *lines[] = { "Hello Module", "", "Hello, world!",
                              "", "hold=back" };
    jpp_sdk_set_frame(ctx, lines, 5);

    jpp_sdk_key_event_t key;
    while (true) {
        jpp_sdk_wait_key(ctx, 0, &key);
        if (key == JPP_SDK_KEY_BACK) break;
    }
}
```

---

## Constraints and limits

!!! warning "One module at a time, and it must be your own."
    A module is loaded from the *host app's own* scoped directory and runs with
    the host app's capabilities — it is your code, not a third party's, which is
    why the SDK does not gate it. It cannot load a module of its own, and
    loading a second one replaces the first.

| Constraint | Value |
|------------|-------|
| Modules resident at once | 1 |
| Module size limit | Remaining pool space after hub image |
| Module location | Must be in the app's scoped directory (`/sd/apps/<app_id>/`) |
| Module can call SDK? | Yes — uses the hub's `ctx` |
| Module can load another module? | No |
| Hub keeps running during module? | Yes — `module_run` blocks; hub resumes when module returns |
| Module auto-unloaded on app exit? | Yes |
