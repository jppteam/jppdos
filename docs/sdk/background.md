# Background tasks and code modules

Two unrelated ways for an app to run code outside its own foreground loop:
headless **background tasks** on a schedule, and **code modules** paged into the
app pool on demand.

## Background registration

!!! warning "Capability: `background.register`."
    **Tier 1** — prompted once, then persisted. The schedule itself lives in the
    manifest's [`background` block](../manifest.md#background); this capability
    only authorises enrolling it.

!!! danger "Background runs cannot prompt."
    A headless run has no screen, so every consent prompt is denied outright.
    Only tier-1 capabilities already granted in a previous *foreground* launch
    are usable, and the task must finish inside a 30-second quota or the
    firmware kills it by restarting the device.

### `background_register`

Trigger the consent prompt for background task scheduling. The schedule itself is defined in `manifest.json`; this call only asks the user to approve it.

/// tab | C
```c
jpp_sdk_status_t jpp_sdk_background_register(jpp_sdk_context_t *ctx,
                                              jpp_broker_result_t *result);
```
///

/// tab | MicroPython
```python
jppsdk.background_register() -> None
```
///

**Notes:**
- Call this once at app startup if background tasks are important to the app's core function.
- If the user denies: `ACCESS_DENIED` / `SdkPermissionError`. The app keeps running without background tasks.
- If already granted (persisted from a prior launch): returns `OK` immediately without showing a prompt.
- After granting, the firmware syncs the app's background tasks into the scheduler every time the app exits. Tasks are removed from the scheduler if the manifest stops declaring them.

---

## Code modules (native apps only)

!!! success "No capability required."
    A module is the app's own code, loaded from the app's own scoped directory
    and run with the host app's capabilities. Nothing to declare, nothing to
    prompt for.

See [Native code modules](../native/modules.md) for the full guide — how the
app pool is split, what a module may and may not do, and a worked plugin
example. Quick reference:

### `jpp_sdk_module_load`

Load a `.mod.bin` from the app's scoped directory into the tail of the app pool,
after the host app's own image. One module at a time.

```c
jpp_sdk_status_t jpp_sdk_module_load(jpp_sdk_context_t *ctx,
                                      const char *relative_path,
                                      void **out_module);
```

### `jpp_sdk_module_run`

Call the module's `jpp_module_entry(ctx, api)`. Blocks until the module returns.
`api` is an app-defined function table the host hands to its module.

```c
jpp_sdk_status_t jpp_sdk_module_run(jpp_sdk_context_t *ctx,
                                     void *module,
                                     void *api);
```

### `jpp_sdk_module_unload`

Unload the module and release the pool tail.

```c
jpp_sdk_status_t jpp_sdk_module_unload(jpp_sdk_context_t *ctx,
                                        void *module);
```

**Notes:** The host app keeps running if a module fails to load, and a module
still resident when the host app is freed is reclaimed automatically.

---
