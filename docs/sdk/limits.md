# Resource limits

Every hard cap in the SDK, in one place. These are compile-time constants in the
firmware, not tunables — an app cannot raise them.

## Display and input

| Resource | Limit |
|----------|-------|
| Frame text rows | 7 |
| Frame row width | ~21 characters |
| Canvas (windowed) | 128×48 pixels |
| Canvas (fullscreen) | 128×64 pixels |
| Input return value | 64 characters |

## Storage

| Resource | Limit |
|----------|-------|
| Open file handles | 4 per session |
| KV key length | 64 characters |
| KV value length | 256 characters |
| IPC payload | 512 bytes |
| File path length | 160 characters |

## Wireless

| Resource | Limit |
|----------|-------|
| BLE scan results | 20 |
| BLE advertisement payload | 31 bytes |
| BLE device name | 32 characters |
| BLE simultaneous connections | 2 per session |
| ESP-NOW payload | 250 bytes |
| TCP listener | 1 |
| TCP accepted connections | 2 |
| TCP recv buffer | 1024 bytes per call |
| HTTPS request body | 2048 bytes |

## Apps and background

| Resource | Limit |
|----------|-------|
| Background tasks (device-wide) | 8 |
| Background task run quota | 30 seconds |
| Background task minimum interval | 60 seconds |
| Manifest capabilities | 16 |
| App pool — firmware v1.1 and earlier | 64 KB |
| App pool — firmware exporting SDK level 3 | 80 KB |

The **app pool** is a single workspace the firmware hands to one foreground
activity at a time: a native app's binary (plus one loaded
[module](../native/modules.md)), or a MicroPython app's GC heap. It is the one
limit on this page that is not the same on every device — it grew from 64 KB to
80 KB alongside [SDK level 3](../sdk-changelog.md#the-app-pool-grew-to-80-kb).

!!! warning "The pool size cannot be required from a manifest."
    There is no `sdk_min` for it and no other way to say "needs 80 KB". An app
    built to fill the larger pool simply fails to load on v1.1 with
    `NO_MEMORY`. Size your binary or heap for **64 KB** unless you control which
    firmware the device runs — or keep the resident part small and put the bulk
    in a module you load only when it fits.
