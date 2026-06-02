# JPPDOS ESP-IDF project

This repo is an ESP-IDF project for the J++Device firmware. The build layout,
flash partitions, and toolchain pins are defined here.

## Version pins

- ESP-IDF: `v5.5.1`

The machine-readable copy of these pins lives in `toolchain_pins.json` at the
repo root.

## Project layout

- `CMakeLists.txt`: top-level ESP-IDF project entrypoint
- `sdkconfig.defaults`: default ESP32-C6 and partition-table settings
- `partitions.csv`: app plus flash-backed runtime partitions for the firmware
- `main/`: the `app_main()` entrypoint that boots the firmware
- `components/jpp_core/`: the native core implementation used by `main/`

## Canonical commands

Build from the repo root with the Docker toolchain:

```bash
docker compose run --rm build idf.py set-target esp32c6
docker compose run --rm build idf.py build
```

Flash and monitor on hardware:

```bash
docker compose run --rm build idf.py flash monitor
```

Run the host-side document and manifest checks under `tests/`:

```bash
python3 tests/check_architecture_docs.py
python3 tests/check_esp_idf_contract.py contract
python3 tests/validate_manifests.py apps
```

## Partition assumptions

- `factory`: the ESP-IDF application image
- `data_fs`: flash-backed writable data region backing `/data`
- `runtime_fs`: flash-backed runtime/library region backing `/lib`
- `/sd`: removable media, not represented in the on-chip partition table
