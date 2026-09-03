#!/usr/bin/env python3
"""
Compile TestAppMP main.py → main.mpy and stage artefacts to
build/apps/testapp_mp/.

Invoked by the 'testapp_mp_bin' CMake custom target inside idf.py build, or
directly:  python3 apps/testapp_mp/build_mp.py <build_dir> <src_dir>

Requires mpy-cross 1.28.0 (bytecode ABI 6).  Looked up in this order:
  1. mpy-cross on PATH
  2. mpy-cross-v1.28.0 on PATH
  3. $IDF_PATH/../mpy-cross/mpy-cross  (common IDF Docker layout)
  4. A local mpy-cross binary next to this script
"""

import os
import shutil
import subprocess
import sys
from typing import Optional


def _find_mpy_cross(script_dir: str) -> Optional[str]:
    for name in ("mpy-cross", "mpy-cross-v1.28.0"):
        found = shutil.which(name)
        if found:
            return found
    idf_path = os.environ.get("IDF_PATH", "")
    if idf_path:
        candidate = os.path.normpath(
            os.path.join(idf_path, "..", "mpy-cross", "mpy-cross")
        )
        if os.path.isfile(candidate):
            return candidate
    local = os.path.join(script_dir, "mpy-cross")
    if os.path.isfile(local):
        return local
    return None


def main(build_dir: str, src_dir: str) -> int:
    src_py = os.path.join(src_dir, "main.py")
    src_manifest = os.path.join(src_dir, "manifest.json")

    if not os.path.exists(src_py):
        print(f"ERROR: source not found: {src_py}", file=sys.stderr)
        return 1
    if not os.path.exists(src_manifest):
        print(f"ERROR: manifest not found: {src_manifest}", file=sys.stderr)
        return 1

    mpy_cross = _find_mpy_cross(src_dir)
    if mpy_cross is None:
        print(
            "ERROR: mpy-cross not found.\n"
            "  PyPI has no 1.28.0 release; the rc emits the same mpy v6.3:\n"
            "    pip install mpy-cross==1.28.0rc0.post2\n"
            "  or build v1.28.0 from source (what the project Dockerfile does).\n"
            "  then re-run:  idf.py build",
            file=sys.stderr,
        )
        return 1

    out_dir = os.path.join(build_dir, "apps", "testapp_mp")
    os.makedirs(out_dir, exist_ok=True)
    out_mpy = os.path.join(out_dir, "main.mpy")
    out_manifest = os.path.join(out_dir, "manifest.json")

    print(f"  MPY main.py")
    cmd = [mpy_cross, "-march=rv32imc", "-O2", "-o", out_mpy, src_py]
    result = subprocess.run(cmd)
    if result.returncode != 0:
        print(f"ERROR: mpy-cross failed for {src_py}", file=sys.stderr)
        return result.returncode

    shutil.copy2(src_manifest, out_manifest)
    print("  CP  manifest.json")

    mpy_size = os.path.getsize(out_mpy)
    print(f"OK  {out_mpy}  ({mpy_size} bytes)")
    print()
    print("To deploy: copy main.mpy and manifest.json to /sd/apps/testapp_mp/ on the SD card.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <build_dir> <src_dir>", file=sys.stderr)
        sys.exit(1)
    sys.exit(main(sys.argv[1], sys.argv[2]))
