from __future__ import annotations

import argparse
from pathlib import Path
from typing import cast


ROOT = Path(__file__).resolve().parents[1]
CONTRACT_PATH = ROOT / "ESP_IDF_CONTRACT.md"
REQUIRED_HEADINGS = (
    "Compatibility matrix",
    "Boot order",
    "Settings v2",
    "Logical paths",
    "Broker ownership",
    "SSD1306 text UI parity",
    "Single shared VM",
    "Best-effort sandbox",
    "Manifest v2 package model",
    "SDK surface and versioning",
    "App lifecycle",
    "Failure taxonomy",
    "Out-of-scope items",
)
REQUIRED_MATRIX_HEADINGS = (
    "Preserved behavior",
    "Changed behavior",
    "Removed behavior",
)
REQUIRED_MANIFEST_FIELDS = (
    "schema_version",
    "app_id",
    "name",
    "version",
    "sdk_min",
    "sdk_max",
    "entry",
    "capabilities",
    "background.enabled",
    "background.mode",
    "modules",
    "toolchain.runtime_version",
    "toolchain.cross_version",
    "toolchain.bytecode_abi",
)
REQUIRED_FAILURE_CODES = (
    "INVALID_JSON",
    "INVALID_MANIFEST",
    "SCHEMA_MISMATCH",
    "INVALID_ENTRY",
    "SDK_MISMATCH",
    "DUPLICATE_APP_ID",
    "ENTRY_MISSING",
    "ENTRY_CORRUPT",
    "RUNTIME_MISMATCH",
    "UNSUPPORTED_IMPORT",
    "CREATE_APP_FAILED",
    "APP_CRASH",
    "APP_BG_ERROR",
    "ACCESS_DENIED",
    "SD_REMOVED",
    "STORAGE_FULL",
)
PROHIBITED_SCOPE_TERMS = (
    "OTA",
    "LVGL redesign",
    "multi-VM",
    "multi-foreground-app",
)


def _load_contract() -> tuple[str, list[str]]:
    text = CONTRACT_PATH.read_text(encoding="utf-8")
    return text, text.splitlines()


def _heading_title(line: str) -> tuple[int, str] | None:
    stripped = line.strip()
    if not stripped.startswith("#"):
        return None
    level = len(stripped) - len(stripped.lstrip("#"))
    return level, stripped[level:].strip()


def _missing_labels(available: set[str], required: tuple[str, ...]) -> list[str]:
    return [label for label in required if label.lower() not in available]


def run_contract_check() -> int:
    text, lines = _load_contract()
    heading_titles: set[str] = set()
    for line in lines:
        heading = _heading_title(line)
        if heading is not None:
            heading_titles.add(heading[1].lower())
    normalized = text.lower()

    missing_headings = _missing_labels(heading_titles, REQUIRED_HEADINGS)
    missing_matrix = _missing_labels(heading_titles, REQUIRED_MATRIX_HEADINGS)
    missing_fields = [field for field in REQUIRED_MANIFEST_FIELDS if field.lower() not in normalized]
    missing_codes = [code for code in REQUIRED_FAILURE_CODES if code.lower() not in normalized]

    print(f"CONTRACT_PATH|{CONTRACT_PATH.relative_to(ROOT)}")
    print(f"LINES|{len(lines)}")
    print(f"HEADINGS_PRESENT|{len(REQUIRED_HEADINGS) - len(missing_headings)}/{len(REQUIRED_HEADINGS)}")
    print(f"MATRIX_HEADINGS_PRESENT|{len(REQUIRED_MATRIX_HEADINGS) - len(missing_matrix)}/{len(REQUIRED_MATRIX_HEADINGS)}")
    print(f"MANIFEST_FIELDS_PRESENT|{len(REQUIRED_MANIFEST_FIELDS) - len(missing_fields)}/{len(REQUIRED_MANIFEST_FIELDS)}")
    print(f"FAILURE_CODES_PRESENT|{len(REQUIRED_FAILURE_CODES) - len(missing_codes)}/{len(REQUIRED_FAILURE_CODES)}")

    errors: list[str] = []
    errors.extend(f"MISSING_HEADING|{label}" for label in missing_headings)
    errors.extend(f"MISSING_MATRIX_HEADING|{label}" for label in missing_matrix)
    errors.extend(f"MISSING_MANIFEST_FIELD|{field}" for field in missing_fields)
    errors.extend(f"MISSING_FAILURE_CODE|{code}" for code in missing_codes)
    if errors:
        for item in errors:
            print(item)
        return 1

    print("CONTRACT_CHECK_OK")
    return 0


def run_scope_check() -> int:
    _, lines = _load_contract()
    current_h2 = ""
    current_h3 = ""
    seen_terms = {term: 0 for term in PROHIBITED_SCOPE_TERMS}
    violations: list[str] = []

    print(f"CONTRACT_PATH|{CONTRACT_PATH.relative_to(ROOT)}")
    for line_number, line in enumerate(lines, start=1):
        heading = _heading_title(line)
        if heading is not None:
            level, title = heading
            if level == 2:
                current_h2 = title
                current_h3 = ""
            elif level == 3:
                current_h3 = title
        lowered = line.lower()
        for term in PROHIBITED_SCOPE_TERMS:
            if term.lower() not in lowered:
                continue
            seen_terms[term] += 1
            allowed = current_h2 == "Out-of-scope items" or current_h3 == "Removed behavior"
            print(
                "SCOPE_TERM|term=%s|line=%s|h2=%s|h3=%s|allowed=%s"
                % (term, line_number, current_h2 or "-", current_h3 or "-", int(allowed))
            )
            if not allowed:
                violations.append(f"SCOPE_VIOLATION|term={term}|line={line_number}|h2={current_h2}|h3={current_h3}")

    missing_terms = [term for term, count in seen_terms.items() if count == 0]
    for term in missing_terms:
        print(f"MISSING_SCOPE_TERM|{term}")
    for violation in violations:
        print(violation)
    if missing_terms or violations:
        return 1

    print("SCOPE_CHECK_OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate the ESP-IDF contract document.")
    _ = parser.add_argument(
        "mode",
        choices=("contract", "scope"),
        help="Run the required heading/content check or the out-of-scope placement check.",
    )
    args = parser.parse_args()
    mode = cast(str, args.mode)
    if mode == "contract":
        return run_contract_check()
    return run_scope_check()


if __name__ == "__main__":
    raise SystemExit(main())
