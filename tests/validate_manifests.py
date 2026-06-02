import argparse
import importlib
import sys
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Protocol, cast

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

if TYPE_CHECKING:
    from firmware.runtime.discovery import discover_sd_apps as _discover_sd_apps  # pyright: ignore[reportMissingImports, reportUnknownVariableType, reportUnusedImport]
    from firmware.storage import Storage as _Storage  # pyright: ignore[reportMissingImports, reportUnknownVariableType, reportUnusedImport]


class _StorageLike(Protocol):
    sd_mounted: bool


class _StorageFactory(Protocol):
    def __call__(self, *, flash_root: str, sd_root: str) -> _StorageLike: ...


class _DiscoverSDApps(Protocol):
    def __call__(
        self,
        storage: _StorageLike,
        /,
    ) -> tuple[list[dict[str, object]], list[dict[str, object]], list[dict[str, object]]]: ...


def main() -> None:
    parser = argparse.ArgumentParser()
    _ = parser.add_argument("apps_root")
    args = parser.parse_args()

    apps_root = Path(cast(str, args.apps_root)).resolve()
    sd_root = apps_root.parent

    discovery_module = importlib.import_module("firmware.runtime.discovery")
    storage_module = importlib.import_module("firmware.storage")
    discover_sd_apps = cast(_DiscoverSDApps, getattr(discovery_module, "discover_sd_apps"))
    Storage = cast(_StorageFactory, getattr(storage_module, "Storage"))

    with tempfile.TemporaryDirectory() as flash_root:
        storage = Storage(flash_root=flash_root, sd_root=str(sd_root))
        storage.sd_mounted = True
        enabled_apps, disabled_apps, rejected_apps = discover_sd_apps(storage)

    valid_apps = [*enabled_apps, *disabled_apps]

    print("VALIDATE_MANIFESTS|valid=%s|rejected=%s" % (len(valid_apps), len(rejected_apps)))
    for app in valid_apps:
        print("APP_VALID|app_id=%s|entry=%s" % (app.get("app_id", ""), app.get("entry", "")))
    for rejected in rejected_apps:
        print(
            "APP_INVALID|app_dir=%s|reason=%s|details=%s" % (
                rejected.get("app_dir", ""),
                rejected.get("reason", ""),
                rejected.get("details", ""),
            )
        )
    if rejected_apps:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
