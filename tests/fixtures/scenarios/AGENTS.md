# SCENARIO FIXTURE KNOWLEDGE BASE

## DOCUMENTATION MAINTENANCE (NON-NEGOTIABLE)
This file MUST be updated in the same change as any fixture layout, scenario
naming, or fixture-behaviour change it describes. Keeping it current is
non-negotiable and non-deferrable.

## OVERVIEW
Scenario corpus for testing only. Each scenario mirrors device state through `flash/` and optional `sd/` trees for future verification and regression coverage.

## STRUCTURE
```text
fixtures/scenarios/<scenario>/
├── flash/
│   ├── data/             # persisted settings, crash logs
│   ├── lib/              # optional library tree
│   └── .keep             # placeholder for empty roots
└── sd/
    └── apps/<app_id>/
        ├── manifest.json
        └── entry
```

## WHERE TO LOOK
| Task | Location | Notes |
|---|---|---|
| Canonical good boot | `boot_valid_sd/` | mounted SD + valid app |
| Manifest edge cases | `manifest_matrix/` | duplicate IDs, missing entry, SDK mismatch |
| Launcher / crash behavior | `launcher_navigation/`, `app_crash_recovery/` | UI-specific flows |
| Persistence checks | `settings_persist/`, `settings_corrupt_recovery/` | stored settings variants |
| Background watchdog | `bg_starvation_guard/` | long-running background task fixture |

## CONVENTIONS
- Scenario directory names map directly to scenario runner names and future test helpers.
- `flash/` models on-device persisted state; `sd/` models removable app storage.
- App fixtures are intentionally tiny and deterministic.
- `manifest_matrix/` is the dense schema/rejection corpus; keep it explicit rather than clever.
- Fixtures remain in the repo even though `tests/` has been removed; keep them ready for future testing work.

## ANTI-PATTERNS
- Do not add fixture-only rules to root docs when they only matter here.
- Do not make scenario apps depend on undeclared capabilities or extra package structure.
- Do not mutate shared baseline fixtures when a scenario-specific copy is clearer.
- Do not assume every directory under `fixtures/scenarios/` is active; verify it is referenced by the runner.

## NOTES
- `boot_corrupt_settings/` currently exists outside the active scenario map.
- The baseline good app shape also appears under `fixtures/sdcard/apps/demo_clock/`.
- Some scenarios intentionally include malformed JSON, missing files, or crash logs; preserve those failures as test data.
