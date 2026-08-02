# SDK changelog

Every change to the App SDK surface, by API level.

This is **not** the firmware changelog. It tracks one number — the SDK API level
the firmware exports — and answers a single question: *what do I put in
`sdk_min`?*

---

## How levels work

The firmware exports exactly one SDK API level (`JPP_SDK_VERSION` in
`jpp_manifest_core.h`). An app declares the lowest level it can run on in its
manifest:

```json
{ "sdk_min": 2 }
```

At launch the loader compares the two. If `sdk_min` is greater than the level
the running firmware exports, the app is rejected with `SDK_TOO_OLD` and shows
up as `MANIFEST_REJECTED` / `LAUNCH_FAILED` on screen.

!!! info "`sdk_min` is a floor, never a ceiling."
    There is no `sdk_max` — it existed briefly and was removed. The SDK surface
    only ever grows in a backward-compatible way: new functions are appended,
    new struct fields go at the tail, and enumerators are never renumbered. An
    app built for level 1 keeps working on every later firmware, unchanged and
    un-rebuilt.

So the rule is simply: **declare the lowest level that provides every symbol and
capability you actually use.** Declaring a higher level than you need only
narrows the set of devices that will run your app.

| `sdk_min` | Runs on | Notes |
|-----------|---------|-------|
| `1` | every shipped unit | The safe default |
| `2` | firmware v1.1 and later | Required for TLS, outbound TCP, crypto, and CENTER claims |
| `3` | *no released firmware yet* | Required only for `wrap_text` |

---

## Level 3

**Unreleased** · open

!!! warning "No firmware has shipped this level yet."
    An app declaring `sdk_min: 3` will be rejected with `SDK_TOO_OLD` on every
    unit in the field, including v1.1. Do not ship one until a firmware release
    exports level 3.

### `jpp_sdk_wrap_text` became callable

[`wrap_text`](sdk/display.md#wrap_text) was declared in the level-1 header and
documented as a native SDK call from v1.0-RTM onward, but it was never listed in
the firmware's native symbol table — so a native app that called it was rejected
at launch with `UNRESOLVED_SYM`. It now resolves.

This is the same defect that affected [`confirm`](#jpp_sdk_confirm-became-callable)
at level 2, and it is handled the same way: the surface genuinely changed from
an app's point of view, so it mints a level rather than being folded into a
closed one. If your native app calls `wrap_text`, declare `sdk_min: 3` — a
level-2 device cannot run it regardless of what the level-1 header advertised.

- No capability — pure computation on caller-supplied buffers.
- New symbol in `s_symtab`: `jpp_sdk_wrap_text`. The function itself is
  unchanged; only its reachability from a loaded app binary is new.
- **C only.** MicroPython apps are unaffected — they never resolve symbols
  through `s_symtab`.

### The app pool grew to 80 KB

The single pool your app is loaded into — code for a native app, GC heap for a
MicroPython one — went from 64 KB to **80 KB**. Nothing about the API changed,
so this needs no `sdk_min` of its own: a bigger pool cannot break an app, and
`sdk_min: 3` already implies a firmware that has it.

It matters if you were up against the ceiling. A native hub plus one module now
has 16 KB more to play with (see [Code modules](native/modules.md)), and a
MicroPython app has a larger GC heap before collection pressure starts to bite.

!!! warning "It does not mean a level-2 device will load a bigger app."
    The pool size is a property of the firmware, not of the SDK level, and
    there is no way to declare "needs an 80 KB pool" in a manifest. An app built
    to fill 80 KB simply fails to load on firmware v1.1 with `NO_MEMORY`. If
    that matters to you, keep the binary under 64 KB or gate the extra bulk
    behind a module you load only when it fits.

---

## Level 2

**Firmware v1.1** · released 2026-07-29

Three independent additions plus the input-gesture work landed in the same
release, so they all share one level.

### Outbound TCP — `network.connect`

A client counterpart to the existing `network.bind` listener.
[`net_connect`](sdk/network.md#net_connect) resolves a host and connects,
returning a socket usable with the *same* `net_recv` / `net_send` / `net_close`
calls as an accepted server socket. Those three now accept a socket obtained
from either capability; holding either one is enough to move bytes on a socket
you own. Connected sockets share the 2-entry connection table with accepted
ones.

- New capability: `network.connect` (tier 2 — per-session, never persisted).
- New symbol: `jpp_sdk_net_connect`.
- **C only.** There is no `jppsdk.net_connect` binding.

### TLS-verified HTTP — `https.request`

[`https_request`](sdk/network.md#https_request) does GET and POST over TLS with
the server certificate verified against the firmware's bundled CA roots.
Verification cannot be disabled — there is no insecure flag.

Consent is deliberately two-stage: the tier-1 capability prompt, and then a
**per-origin** prompt naming each `scheme://host[:port]` the app contacts.
Approved origins persist to `/data/grants/<app_id>.origins`, so the user is
asked once per host. See [Per-origin
consent](sdk/network.md#per-origin-consent).

- New capability: `https.request` (tier 1 — persisted), plus the per-origin grant.
- New symbol: `jpp_sdk_https_request`.
- MicroPython binding: `jppsdk.https_request`.
- Shares the broker's HTTP lock with `http_request` — one request in flight at a time.

### Crypto primitives

Stateless, mbedTLS-backed [crypto primitives](sdk/crypto.md), hardware
accelerated on the ESP32-C6: `jpp_crypto_sha256`, `jpp_crypto_sha1`,
`jpp_crypto_aes256_ige_encrypt` / `_decrypt`, `jpp_crypto_modexp`,
`jpp_crypto_rsa_encrypt`, and `jpp_crypto_dh_compute`.

They exist so an app can do transport crypto without carrying AES and bignum
code inside the app pool. The MTProto client skeleton is the reference
user: it fits in roughly 11 KB of pool because the heavy crypto stayed in the
firmware.

- No capability — pure computation, nothing to gate.
- **C only**, and they are plain functions from `jpp_crypto_core.h` rather than
  `jpp_sdk_*` calls.

### CENTER gesture claims

The device gained a user preference for whether **hold** or **double-click**
means "Back" (Settings → Controls). Apps never read that preference. Instead:

- Claim nothing (the default) and you receive `JPP_SDK_KEY_BACK` whenever the
  user asks to go back, with the firmware deciding which physical gesture that
  was.
- Claim a gesture with [`claim_center`](sdk/app-control.md#claim_center) and it
  becomes yours, arriving as `JPP_SDK_KEY_CENTER_HOLD` or
  `JPP_SDK_KEY_CENTER_DOUBLE` — and your app then owns its own way out.

New symbol `jpp_sdk_claim_center`; new enumerators `JPP_SDK_KEY_CENTER_HOLD`
and `JPP_SDK_KEY_CENTER_DOUBLE`; new constants `JPP_SDK_CENTER_CLAIM_NONE` /
`_HOLD` / `_DOUBLE`. `JPP_SDK_KEY_BACK` is an **alias** of the pre-existing
`JPP_SDK_KEY_CENTER_LONG` — same value, better name — so code using the old
spelling is unaffected. Bound in MicroPython as `jppsdk.claim_center` with the
matching `KEY_*` / `CENTER_CLAIM_*` constants.

### `jpp_sdk_confirm` became callable

[`confirm`](sdk/display.md#confirm) was declared in the level-1 header but was
missing from the firmware's native symbol table, so a native app that called it
was rejected at launch with `UNRESOLVED_SYM`. It resolves correctly from v1.1
onward. If your native app uses it, declare `sdk_min: 2` — a level-1 device
cannot run it regardless of what the level-1 header advertised.

### Under the hood

`jpp_sdk_native_services_t` is **frozen** at its level-1 shape. It is embedded
by value near the top of `jpp_sdk_context_t`, above fields that app binaries
read directly, so growing it — even at its own tail — shifts offsets in
already-deployed `.bin` files with no load-time error. Post-v1 service callbacks
now live in `jpp_sdk_services_v2_t` at the tail of the context instead. This
does not change anything an app writes, but it is why new callbacks appear where
they do.

---

## Level 1

**Firmware v1.0-RTM** · the original surface

Everything the SDK shipped with, and still the right `sdk_min` for any app that
does not need a level-2 feature:

- **App control and input** — `set_frame`, `request_close`, `log`,
  `request_cap`, `poll_key`, `wait_key`, `push_key`.
- **Canvas** — `canvas_write`, `canvas_draw_pixel`, `canvas_clear`,
  `canvas_fullscreen`, windowed 128×48 and fullscreen 128×64.
- **UI helpers** — `dialog`, `list`, `input`, `file_pick`.
- **Buzzer, LED, wakelock** — the `buzzer_*` family including the async
  sequence player, `led_set_color` / `led_off`, `wakelock_acquire` / `_release`.
- **Device status** — `device_status`, `get_time`, `is_dummy_mode`.
- **Storage** — scoped and shared file I/O, the `files.full` handle API, the
  key-value store, and IPC mailboxes.
- **Network** — `http.request` and the `network.bind` TCP listener.
- **BLE** — scan, advertise, GATT client, GATT server.
- **ESP-NOW** — `espnow_send` / `espnow_recv`.
- **Background** — `background_register` and the manifest schedule.
- **Code modules** — `module_load` / `module_run` / `module_unload`.

---

## For firmware contributors

!!! danger "One level per released firmware."
    Level 2 absorbed four separate additions only because no released firmware
    had ever exported it — no app could have been built against a partial
    version of it. That window is now shut. Level 2 shipped in v1.1 and is
    **closed**: folding a fifth addition into it would leave `sdk_min: 2`
    meaning two different surfaces in the field.

    **Level 3 is currently open.** No firmware has shipped it, so further
    additions made before the next release belong in level 3 — do not mint
    level 4 for them.

Pick the number as **(last released level) + 1**, not (master + 1) and not the
next unused integer. A level is closed by a *release*, not by being merged, so
every branch in flight targets the same number and they converge by
construction. Two branches that both mint level 3 merge cleanly to `3`.

!!! warning "Re-target your level if a release lands while your branch is open."
    This is the one case git cannot catch: both sides agree on the number, so
    the merge succeeds silently and your addition ends up inside a level that
    has already shipped — reintroducing the `UNRESOLVED_SYM` class of bug with
    no up-front rejection. Rebasing past a release means re-checking this
    number by hand.

When you do add to the surface:

1. Bump `JPP_SDK_VERSION` in `components/jpp_core/include/jpp_manifest_core.h`.
2. Mirror it in `tests/validate_manifests.py` (`SDK_VERSION`, and
   `ALLOWED_CAPABILITIES` for a new capability). `tests/test_sdk_abi.py` fails
   the host tests if the two disagree — a guard added after `network.connect`
   shipped in the Python mirror without ever reaching the C whitelist.
3. Add every new `jpp_sdk_*` function to `s_symtab` in
   `components/jpp_native_loader_core/src/jpp_native_symtab.c`, or native apps
   calling it die at launch with `UNRESOLVED_SYM`.
4. Append — never insert. New struct fields go at the tail of
   `jpp_sdk_context_t`, new enumerators at the end of the enum, new service
   callbacks in `jpp_sdk_services_v2_t`. Native apps are separately-built ELFs
   that read these offsets directly and get no load-time layout check.
5. Add a level section to this page, and update the
   [`sdk_min` table](manifest.md#sdk_min).
