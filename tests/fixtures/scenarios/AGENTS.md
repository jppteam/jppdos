# SCENARIO FIXTURE KNOWLEDGE BASE

## DOCUMENTATION MAINTENANCE (NON-NEGOTIABLE)
This file MUST be updated in the same change as any fixture layout, scenario
naming, or fixture-behaviour change it describes. Keeping it current is
non-negotiable and non-deferrable.

## OVERVIEW
Scenario corpus for testing only. Each scenario mirrors device state through `flash/` and optional `sd/` trees for verification and regression coverage.

## STRUCTURE
```text
fixtures/scenarios/<scenario>/
├── flash/
│   └── data/             # persisted settings, crash logs
└── sd/
    └── apps/<app_id>/
        ├── manifest.json
        └── entry artefact (main.mpy / main.bin)
```

## WHERE TO LOOK
| Task | Location | Notes |
|---|---|---|
| Canonical good boot | `boot_valid_sd/` | v2 settings + valid app package |
| Boot without SD | `boot_no_sd/` | v2 settings, no `sd/` tree |
| Manifest edge cases | `manifest_matrix/` | one package per validator outcome; expectations asserted by `tests/test_manifests.py` |
| Settings recovery | `boot_corrupt_settings/` (non-v2 schema), `settings_corrupt_recovery/` (truncated JSON) | both must trigger `SETTINGS_RESET` on device |
| Crash log shape | `app_crash_recovery/` | `/data/ui_crash.log` lines: `app=<id> reason=<reason>` |

## MANIFEST_MATRIX CORPUS
One package directory per outcome of `tests/validate_manifests.py` (which
mirrors `jpp_manifest_core` + the loader preflight):
`valid_mp` and `valid_native` validate; `bad_json` → INVALID_JSON, `schema_v1`
→ SCHEMA_MISMATCH, `settings` → RESERVED_APP_ID (directory name collides with
a builtin screen), `bad_entry` → INVALID_ENTRY, `wrong_suffix` →
INVALID_APP_TYPE, `bad_capability` → INVALID_CAPABILITY, `bad_background` →
INVALID_BACKGROUND, `missing_toolchain` → INVALID_TOOLCHAIN,
`runtime_mismatch` → RUNTIME_MISMATCH, `missing_entry` → MISSING_ENTRY,
`empty_entry` → CORRUPT, `missing_manifest` → MANIFEST_MISSING.
`tests/test_manifests.py::MATRIX_EXPECTED` is the authoritative outcome map —
update it in the same change as any corpus edit.

## CONVENTIONS
- `flash/` models on-device persisted state; `sd/` models removable app storage.
- Valid settings fixtures use the firmware's pruned v2 shape: `schema_version` + `policy.wifi` + `policy.recovery` only.
- Entry artefacts are placeholder text — host checks assert presence and size, not bytecode.
- `manifest_matrix/` is the dense schema/rejection corpus; keep it explicit rather than clever.
- Host-side checks live in `tests/` (`pytest tests` runs them; `validate_manifests.py` is also a standalone CLI).

## ANTI-PATTERNS
- Do not add fixture-only rules to root docs when they only matter here.
- Do not make scenario apps depend on undeclared capabilities or extra package structure.
- Do not mutate shared baseline fixtures when a scenario-specific copy is clearer.

## NOTES
- The full scenario set is: app_crash_recovery, boot_corrupt_settings, boot_no_sd, boot_valid_sd, manifest_matrix, settings_corrupt_recovery.
- The baseline good app shape also appears under `fixtures/sdcard/apps/` (four valid demo packages).
- `boot_corrupt_settings` and `settings_corrupt_recovery` intentionally contain invalid settings payloads; preserve those failures as test data.
