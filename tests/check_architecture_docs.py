from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

REQUIRED_SNIPPETS = {
    "README.md": (
        "J++Device",
        "App SDK",
        "DEVELOPMENT.md",
        "components/jpp_core/",
    ),
    "DEVELOPMENT.md": (
        "App SDK",
        "components/jpp_core/",
        "manifest",
        "capabilities",
        "broker",
    ),
}


def main() -> int:
    errors: list[str] = []
    for relative_path, snippets in REQUIRED_SNIPPETS.items():
        path = ROOT / relative_path
        text = path.read_text(encoding="utf-8")
        print(f"DOC|{relative_path}|bytes={len(text.encode('utf-8'))}")
        for snippet in snippets:
            if snippet in text:
                print(f"OK|{relative_path}|{snippet}")
            else:
                errors.append(f"MISSING|{relative_path}|{snippet}")

    if errors:
        for error in errors:
            print(error)
        return 1

    print("ARCHITECTURE_DOCS_CHECK_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
