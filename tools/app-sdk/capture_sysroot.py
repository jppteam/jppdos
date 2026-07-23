#!/usr/bin/env python3
"""
capture_sysroot.py — distill a JPPDOS app-build sysroot from a firmware build.

Runs in the BUILDER stage of tools/app-sdk/Dockerfile. A full ESP-IDF image is
~12 GB, but compiling an app only needs a compiler plus ~28 MB of headers. This
script works out exactly which headers those are and packages them, so the
final image can start from a slim base instead of the IDF image.

It produces two artefacts:

  <out>/sdk-flags.json   the resolved compiler path + the full, ordered -I list
                         (distilled from build/compile_commands.json, which is
                         ~6.6 MB / 1368 entries — only one entry matters)
  <out>/sysroot.tar      every include directory in that list, at its ABSOLUTE
                         path, minus build artefacts

Absolute paths are preserved so the flags in sdk-flags.json resolve verbatim in
the final image — no path rewriting, nothing to drift.

Usage:  capture_sysroot.py <build_dir> <repo_root> <out_dir>
"""

from __future__ import annotations

import glob
import json
import os
import subprocess
import sys

# Build outputs that can appear inside an include dir (notably the generated
# build/esp-idf/micropython tree: 27 MB of objects around 76 KB of headers).
EXCLUDE_PATTERNS = [
    "*.o", "*.a", "*.obj", "*.d", "*.elf", "*.bin", "*.map",
    "*.lst", "*.S", "*.c", "*.cpp", "*.pyc", "__pycache__",
]


def reference_entry(build_dir: str) -> tuple[list[str], str]:
    """Return (include_flags, compiler) from a jpp_core compile command."""
    cc_path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.isfile(cc_path):
        sys.exit(f"ERROR: {cc_path} not found; run idf.py build first")
    with open(cc_path) as f:
        commands = json.load(f)

    includes: list[str] = []
    for entry in commands:
        if "jpp_sdk_bridge" in entry["file"] or "jpp_core" in entry["file"]:
            for tok in entry["command"].split():
                if tok.startswith("-I") and tok not in includes:
                    includes.append(tok)
            break
    if not includes:
        sys.exit("ERROR: no jpp_core compile entry in compile_commands.json")

    compiler = "riscv32-esp-elf-gcc"
    for entry in commands[:10]:
        toks = entry["command"].split()
        if toks and "riscv" in toks[0]:
            compiler = toks[0]
            break
    return includes, compiler


def main(build_dir: str, repo_root: str, out_dir: str) -> int:
    includes, compiler = reference_entry(build_dir)

    # Mirror jppd-build's own include additions so the runtime never has to
    # re-derive them: every jpp component's public headers + shared app helpers.
    for inc in sorted(glob.glob(os.path.join(repo_root, "components", "*", "include"))):
        flag = f"-I{inc}"
        if flag not in includes:
            includes.append(flag)
    common = os.path.join(repo_root, "apps", "common")
    if os.path.isdir(common) and f"-I{common}" not in includes:
        includes.append(f"-I{common}")

    os.makedirs(out_dir, exist_ok=True)

    flags = {
        "compiler": compiler,
        "include_flags": includes,
        "generated_from": os.path.abspath(build_dir),
    }
    flags_path = os.path.join(out_dir, "sdk-flags.json")
    with open(flags_path, "w") as f:
        json.dump(flags, f, indent=2)
    print(f"  sdk-flags.json: {len(includes)} include dirs, compiler={compiler}")

    # Package the include closure at absolute paths, dropping build artefacts.
    dirs = [i[2:] for i in includes if os.path.isdir(i[2:])]
    missing = [i[2:] for i in includes if not os.path.isdir(i[2:])]
    for m in missing:
        print(f"  WARNING: include dir does not exist, skipping: {m}")

    tar_path = os.path.join(out_dir, "sysroot.tar")
    cmd = ["tar", "-cf", tar_path]
    for pat in EXCLUDE_PATTERNS:
        cmd += ["--exclude", pat]
    # Absolute paths on purpose; tar strips the leading "/" and the Dockerfile
    # untars with -C / to restore them exactly.
    cmd += dirs
    result = subprocess.run(cmd, stderr=subprocess.PIPE, text=True)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        sys.exit("ERROR: failed to package sysroot")

    size_mb = os.path.getsize(tar_path) / (1024 * 1024)
    print(f"  sysroot.tar: {len(dirs)} dirs, {size_mb:.1f} MB")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(f"Usage: {sys.argv[0]} <build_dir> <repo_root> <out_dir>")
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3]))
