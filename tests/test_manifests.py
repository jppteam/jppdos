"""Manifest validation: real app packages and the fixture corpora."""

from pathlib import Path

from validate_manifests import validate_apps_root

ROOT = Path(__file__).resolve().parents[1]
APPS_ROOT = ROOT / "apps"
SDCARD_APPS = ROOT / "tests" / "fixtures" / "sdcard" / "apps"
MATRIX_APPS = ROOT / "tests" / "fixtures" / "scenarios" / "manifest_matrix" / "sd" / "apps"

# Every package in the manifest_matrix corpus and its expected outcome.
# A None reason means the package must validate.
MATRIX_EXPECTED = {
    "valid_mp": None,
    "valid_native": None,
    "bad_json": "INVALID_JSON",
    "schema_v1": "SCHEMA_MISMATCH",
    "settings": "RESERVED_APP_ID",
    "bad_entry": "INVALID_ENTRY",
    "wrong_suffix": "INVALID_APP_TYPE",
    "bad_capability": "INVALID_CAPABILITY",
    "bad_background": "INVALID_BACKGROUND",
    "missing_toolchain": "INVALID_TOOLCHAIN",
    "runtime_mismatch": "RUNTIME_MISMATCH",
    "missing_entry": "MISSING_ENTRY",
    "empty_entry": "CORRUPT",
    "missing_manifest": "MANIFEST_MISSING",
}


def test_repo_app_manifests_are_valid():
    # Entry artefacts (main.mpy, *.bin) are build outputs, so only the
    # manifest rules are checked for the source tree. apps/common/ is shared
    # app-side source (jpp_ble_msg) compiled into other apps, not a package
    # of its own, so it carries no manifest.json and is excluded from scan.
    valid, rejected = validate_apps_root(
        APPS_ROOT, check_entry_file=False, exclude=frozenset({"common"})
    )
    assert rejected == []
    assert {app_id for app_id, _ in valid} == {"demoscene", "games", "meetapp", "testapp_mp", "testapp_native"}


def test_sdcard_fixture_corpus_is_valid():
    valid, rejected = validate_apps_root(SDCARD_APPS)
    assert rejected == []
    assert {app_id for app_id, _ in valid} == {
        "demo_clock",
        "demo_netcheck",
        "demo_notes",
        "demo_sdk_ui",
    }


def test_manifest_matrix_outcomes_match_expectations():
    valid, rejected = validate_apps_root(MATRIX_APPS)
    outcomes: dict[str, str | None] = {app_id: None for app_id, _ in valid}
    outcomes.update({app_dir: reason for app_dir, reason, _ in rejected})
    assert outcomes == MATRIX_EXPECTED
