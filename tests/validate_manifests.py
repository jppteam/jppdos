"""Host-side manifest validator mirroring the firmware rules.

Self-contained reimplementation of the checks in
components/jpp_core/src/jpp_manifest_core.c (jpp_manifest_v2_validate) plus the
entry-file preflight in components/jpp_core/src/jpp_app_loader_core.c
(jpp_app_loader_preflight).  Keep the two in sync: any rule change in the C
sources must be reflected here in the same change.

Usage:  python3 validate_manifests.py <apps_root>

<apps_root> is a directory of app packages (<apps_root>/<dir>/manifest.json).
Like the firmware's discovery, the app id is the directory name; an "app_id"
key inside manifest.json is not consulted.

Output markers (one per line):
  VALIDATE_MANIFESTS|valid=<n>|rejected=<n>
  APP_VALID|app_id=<id>|entry=<entry>
  APP_INVALID|app_dir=<dir>|reason=<reason>|details=<details>

Exit status is 1 when any package is rejected.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Pinned values from components/jpp_core/include/jpp_manifest_core.h
SCHEMA_VERSION = 2
RUNTIME_VERSION = "v1.28.0"
CROSS_VERSION = "1.28.0"
BYTECODE_ABI = 6
BG_TASK_MAX = 4

# From components/jpp_core/include/jpp_resource_budget.h
BG_INTERVAL_MIN_S = 60

# jpp_manifest_v2_is_reserved_app_id
RESERVED_APP_IDS = frozenset(
    (
        "launcher",
        "settings",
        "webdav",
        "webdav_passconfig",
        "shell",
        "dialog",
        "app_crash",
        "sd_ejected",
    )
)

# jpp_manifest_v2_is_allowed_capability — the nine prompted capabilities
ALLOWED_CAPABILITIES = frozenset(
    (
        # Tier 1 — one-time user grant, persisted
        "http.request",
        "ble.scan",
        "ble.advertise",
        "background.register",
        "esp_now",
        # Tier 2 — per-session user grant
        "files.full",
        "network.bind",
        "ble.connect",
        "ble.host",
    )
)

CRON_FIELD_RANGES = ((0, 59), (0, 23), (1, 31), (1, 12), (0, 6))


def _name_valid(name: object) -> bool:
    """Mirror jpp_str_name_valid: [A-Za-z0-9_] segments joined by single dots."""
    if not isinstance(name, str) or not name or name.startswith("."):
        return False
    previous_was_dot = False
    for index, char in enumerate(name):
        if char == ".":
            if previous_was_dot or index == len(name) - 1:
                return False
            previous_was_dot = True
            continue
        if char.isascii() and (char.isalnum() or char == "_"):
            previous_was_dot = False
            continue
        return False
    return True


def _has_parent_segment(path: str) -> bool:
    """Mirror jpp_str_has_parent_segment: any ".." path segment."""
    return ".." in path.split("/")


def _cron_valid(cron: object) -> bool:
    """Mirror jpp_manifest_v2_is_valid_cron: 5 fields, "*" or one number.
    The C parser tolerates repeated and trailing spaces but not leading ones."""
    if not isinstance(cron, str) or not cron or cron.startswith(" "):
        return False
    fields = [field for field in cron.split(" ") if field]
    if len(fields) != len(CRON_FIELD_RANGES):
        return False
    for field, (low, high) in zip(fields, CRON_FIELD_RANGES):
        if field == "*":
            continue
        if not field.isdigit():
            return False
        if not low <= int(field) <= high:
            return False
    return True


def _nonempty_str(value: object) -> bool:
    return isinstance(value, str) and value != ""


def _validate_background(background: object) -> str | None:
    if background is None:
        return None
    if not isinstance(background, dict):
        return "INVALID_BACKGROUND"
    enabled = background.get("enabled", False)
    if not isinstance(enabled, bool):
        return "INVALID_BACKGROUND"
    tasks = background.get("tasks", [])
    if not isinstance(tasks, list) or len(tasks) > BG_TASK_MAX:
        return "INVALID_BACKGROUND"
    for task in tasks:
        if not isinstance(task, dict) or not _name_valid(task.get("name")):
            return "INVALID_BACKGROUND"
        interval = task.get("interval_s", 0)
        cron = task.get("cron", "")
        has_interval = isinstance(interval, int) and interval > 0
        has_cron = _nonempty_str(cron)
        if has_interval == has_cron:  # exactly one schedule source
            return "INVALID_BACKGROUND"
        if has_interval and interval < BG_INTERVAL_MIN_S:
            return "INVALID_BACKGROUND"
        if has_cron and not _cron_valid(cron):
            return "INVALID_BACKGROUND"
    return None


def validate_manifest(app_dir_name: str, manifest: dict[str, object]) -> str | None:
    """Mirror jpp_manifest_v2_validate; returns the rejection reason or None.

    The firmware fills parse defaults (load_sd_manifest in
    main/jpp_app_dispatch.c): schema_version/sdk_min/sdk_max default when
    absent, app_type defaults to micropython, and the app id is the directory
    name.
    """
    schema_version = manifest.get("schema_version", SCHEMA_VERSION)
    if schema_version != SCHEMA_VERSION:
        return "SCHEMA_MISMATCH"

    name = manifest.get("name", "")
    version = manifest.get("version", "")
    entry = manifest.get("entry", "")
    if not (_nonempty_str(name) and _nonempty_str(version) and _nonempty_str(entry)):
        return "INVALID_MANIFEST"
    assert isinstance(entry, str)

    if app_dir_name in RESERVED_APP_IDS:
        return "RESERVED_APP_ID"

    sdk_min = manifest.get("sdk_min", 1)
    sdk_max = manifest.get("sdk_max", 1)
    if not isinstance(sdk_min, int) or not isinstance(sdk_max, int) or sdk_min > sdk_max:
        return "INVALID_MANIFEST"

    if entry.startswith("/") or _has_parent_segment(entry):
        return "INVALID_ENTRY"

    app_type = manifest.get("app_type", "micropython")
    if app_type == "micropython":
        if not entry.endswith(".mpy"):
            return "INVALID_APP_TYPE"
    elif app_type == "native":
        if not entry.endswith(".bin"):
            return "INVALID_APP_TYPE"
    else:
        return "INVALID_APP_TYPE"

    capabilities = manifest.get("capabilities", [])
    if not isinstance(capabilities, list):
        return "INVALID_MANIFEST"
    for capability in capabilities:
        if capability not in ALLOWED_CAPABILITIES:
            return "INVALID_CAPABILITY"

    background_reason = _validate_background(manifest.get("background"))
    if background_reason is not None:
        return background_reason

    if app_type == "micropython":
        toolchain = manifest.get("toolchain")
        if not isinstance(toolchain, dict):
            return "INVALID_TOOLCHAIN"
        runtime_version = toolchain.get("runtime_version", "")
        cross_version = toolchain.get("cross_version", "")
        bytecode_abi = toolchain.get("bytecode_abi", 0)
        if not (
            _nonempty_str(runtime_version)
            and _nonempty_str(cross_version)
            and isinstance(bytecode_abi, int)
            and bytecode_abi > 0
        ):
            return "INVALID_TOOLCHAIN"
        if (
            runtime_version != RUNTIME_VERSION
            or cross_version != CROSS_VERSION
            or bytecode_abi != BYTECODE_ABI
        ):
            return "RUNTIME_MISMATCH"

    return None


def validate_package(
    app_dir: Path, *, check_entry_file: bool = True
) -> tuple[str | None, str]:
    """Validate one app package directory.

    Returns (reason, details); reason is None for a valid package.  With
    check_entry_file the loader preflight's entry checks run too (MISSING_ENTRY
    for an absent entry file, CORRUPT for an empty one) — disable it for
    source-tree packages whose entry artefact is produced by the build.
    """
    manifest_path = app_dir / "manifest.json"
    if not manifest_path.is_file():
        return "MANIFEST_MISSING", "manifest.json not found"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return "INVALID_JSON", str(exc)
    if not isinstance(manifest, dict):
        return "INVALID_JSON", "top-level value is not an object"

    reason = validate_manifest(app_dir.name, manifest)
    if reason is not None:
        return reason, ""

    if check_entry_file:
        entry = manifest["entry"]
        assert isinstance(entry, str)
        entry_path = app_dir / entry
        if not entry_path.is_file():
            return "MISSING_ENTRY", entry
        if entry_path.stat().st_size == 0:
            return "CORRUPT", entry

    return None, ""


def validate_apps_root(
    apps_root: Path, *, check_entry_file: bool = True
) -> tuple[list[tuple[str, str]], list[tuple[str, str, str]]]:
    """Validate every package under apps_root.

    Returns (valid, rejected): valid entries are (app_id, entry); rejected
    entries are (app_dir, reason, details).
    """
    valid: list[tuple[str, str]] = []
    rejected: list[tuple[str, str, str]] = []
    for app_dir in sorted(path for path in apps_root.iterdir() if path.is_dir()):
        reason, details = validate_package(app_dir, check_entry_file=check_entry_file)
        if reason is None:
            manifest = json.loads((app_dir / "manifest.json").read_text(encoding="utf-8"))
            entry = manifest.get("entry", "")
            valid.append((app_dir.name, str(entry)))
        else:
            rejected.append((app_dir.name, reason, details))
    return valid, rejected


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apps_root", help="Directory of app packages.")
    parser.add_argument(
        "--no-entry-check",
        action="store_true",
        help="Skip entry-file presence checks (for source trees whose entry "
        "artefact is produced by the build).",
    )
    args = parser.parse_args()

    apps_root = Path(args.apps_root).resolve()
    if not apps_root.is_dir():
        print(f"APPS_ROOT_MISSING|{apps_root}")
        return 1

    valid, rejected = validate_apps_root(
        apps_root, check_entry_file=not args.no_entry_check
    )

    print(f"VALIDATE_MANIFESTS|valid={len(valid)}|rejected={len(rejected)}")
    for app_id, entry in valid:
        print(f"APP_VALID|app_id={app_id}|entry={entry}")
    for app_dir, reason, details in rejected:
        print(f"APP_INVALID|app_dir={app_dir}|reason={reason}|details={details}")
    return 1 if rejected else 0


if __name__ == "__main__":
    sys.exit(main())
