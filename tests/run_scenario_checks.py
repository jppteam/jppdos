# pyright: reportMissingImports=false, reportMissingTypeStubs=false, reportAttributeAccessIssue=false, reportUnknownVariableType=false, reportUnknownMemberType=false, reportUnknownArgumentType=false, reportExplicitAny=false, reportAny=false, reportUnusedCallResult=false

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_NATIVE_COMMAND = "idf.py build"
DEFAULT_MEMORY_COMMAND = "idf.py size-components"

if str(ROOT) not in sys.path:
    _ = sys.path.insert(0, str(ROOT))


def _write_text(path: Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8")


def _parse_marker_lines(output: str) -> list[dict[str, str]]:
    markers: list[dict[str, str]] = []
    for line in output.splitlines():
        parts = line.split("|")
        if len(parts) < 2:
            continue
        marker: dict[str, str] = {"name": parts[0]}
        for part in parts[1:]:
            if "=" in part:
                k, _, v = part.partition("=")
                marker[k.strip()] = v.strip()
        markers.append(marker)
    return markers


def _run_python_command(label: str, command: list[str]) -> dict[str, Any]:
    completed = subprocess.run(command, cwd=str(ROOT), capture_output=True, text=True, check=False)
    output = completed.stdout + completed.stderr
    return {
        "label": label,
        "command": command,
        "returncode": completed.returncode,
        "status": "passed" if completed.returncode == 0 else "failed",
        "output": output,
    }


def _run_external_command(label: str, command_text: str) -> dict[str, Any]:
    command = shlex.split(command_text)
    try:
        completed = subprocess.run(command, cwd=str(ROOT), capture_output=True, text=True, check=False)
        output = completed.stdout + completed.stderr
        status = "passed" if completed.returncode == 0 else "failed"
        return {
            "label": label,
            "command": command,
            "returncode": completed.returncode,
            "status": status,
            "output": output,
        }
    except (FileNotFoundError, OSError) as exc:
        return {
            "label": label,
            "command": command,
            "returncode": None,
            "status": "blocked",
            "output": f"{exc.__class__.__name__}: {exc}\n",
        }


def _package_validation_result(apps_root: Path) -> dict[str, Any]:
    result = _run_python_command(
        "package_validation",
        [sys.executable, str(ROOT / "tests" / "validate_manifests.py"), str(apps_root)],
    )
    valid_apps: list[dict[str, str]] = []
    invalid_apps: list[dict[str, str]] = []
    for marker in _parse_marker_lines(str(result["output"])):
        name = marker.get("name", "")
        if name == "APP_VALID":
            valid_apps.append(marker)
        if name == "APP_INVALID":
            invalid_apps.append(marker)
    result["valid_apps"] = valid_apps
    result["invalid_apps"] = invalid_apps
    result["invalid_reasons"] = sorted({item.get("reason", "") for item in invalid_apps if item.get("reason")})
    return result


def _overall_status(checks: dict[str, dict[str, Any]], allowed_blocked: set[str], expect_package_failure: bool) -> tuple[str, list[str]]:
    failures: list[str] = []
    blocked: list[str] = []
    for name, result in checks.items():
        status = str(result.get("status", ""))
        if name == "package_validation" and expect_package_failure:
            if status == "failed":
                continue
            failures.append(f"{name}:expected_failure_not_observed")
            continue
        if status == "failed":
            failures.append(name)
            continue
        if status == "blocked" and name not in allowed_blocked:
            failures.append(f"{name}:blocked")
            blocked.append(name)
            continue
        if status == "blocked":
            blocked.append(name)
    if failures:
        return "failed", failures
    if expect_package_failure:
        return ("expected_failure_with_blockers" if blocked else "expected_failure_observed"), []
    return ("pass_with_blockers" if blocked else "passed"), []


def main() -> int:
    parser = argparse.ArgumentParser(description="Run scenario checks: package validation and native build.")
    _ = parser.add_argument("--apps-root", required=True, help="Apps root used for package validation.")
    _ = parser.add_argument("--output-dir", required=True, help="Directory to write check results.")
    _ = parser.add_argument("--allow-blocked", action="append", default=[], choices=("native_component_tests", "memory_report"))
    _ = parser.add_argument("--expect-package-failure", action="store_true")
    _ = parser.add_argument("--require-package-reason", action="append", default=[])
    _ = parser.add_argument("--native-command", default=DEFAULT_NATIVE_COMMAND)
    _ = parser.add_argument("--memory-command", default=DEFAULT_MEMORY_COMMAND)
    args = parser.parse_args()

    output_dir = (ROOT / str(args.output_dir)).resolve() if not str(args.output_dir).startswith("/") else Path(str(args.output_dir)).resolve()
    apps_root = (ROOT / str(args.apps_root)).resolve() if not str(args.apps_root).startswith("/") else Path(str(args.apps_root)).resolve()

    package_result = _package_validation_result(apps_root)
    native_result = _run_external_command("native_component_tests", str(args.native_command))
    memory_result = _run_external_command("memory_report", str(args.memory_command))

    required_reasons = list(args.require_package_reason)
    missing_reasons = [item for item in required_reasons if item not in package_result["invalid_reasons"]]
    if missing_reasons:
        package_result["status"] = "failed"
        package_result["missing_reasons"] = missing_reasons

    checks = {
        "package_validation": package_result,
        "native_component_tests": native_result,
        "memory_report": memory_result,
    }
    overall_status, failures = _overall_status(checks, set(args.allow_blocked), bool(args.expect_package_failure))

    _write_text(output_dir / "package_validation.log", str(package_result["output"]))
    _write_text(output_dir / "native_component_tests.log", str(native_result["output"]))
    _write_text(output_dir / "memory_report.log", str(memory_result["output"]))

    summary = {
        "overall_status": overall_status,
        "failures": failures,
        "allowed_blocked": list(args.allow_blocked),
        "expect_package_failure": bool(args.expect_package_failure),
        "require_package_reason": required_reasons,
        "output_dir": str(output_dir.relative_to(ROOT)) if output_dir.is_relative_to(ROOT) else str(output_dir),
        "checks": checks,
    }
    _write_text(output_dir / "summary.json", json.dumps(summary, indent=2, sort_keys=True) + "\n")
    return 0 if overall_status != "failed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
