#!/usr/bin/env python3
"""
Build TestAppNative as a position-independent ELF32 shared library.

Reads build/compile_commands.json to obtain the exact -I flags that ESP-IDF
uses for jpp_core compilation, then recompiles the TestAppNative sources with
those flags plus -fPIC -shared -mno-relax, producing
build/apps/testapp_native/testapp_native.so.

Invoked by the 'testapp_native_bin' CMake custom target inside idf.py build, or
directly:  python3 apps/testapp_native/build_shared.py <build_dir> <src_dir>
"""

import json
import os
import shutil
import subprocess
import sys


def main(build_dir: str, src_dir: str) -> int:
    cc_path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(cc_path):
        print(f"ERROR: {cc_path} not found; run idf.py build first", file=sys.stderr)
        return 1

    with open(cc_path) as f:
        commands = json.load(f)

    ref_flags: list[str] = []
    for entry in commands:
        if "jpp_sdk_bridge" in entry["file"] or "jpp_core" in entry["file"]:
            tokens = entry["command"].split()
            for tok in tokens:
                if tok.startswith("-I"):
                    if tok not in ref_flags:
                        ref_flags.append(tok)
            break

    if not ref_flags:
        print("ERROR: could not find a jpp_core compile entry in compile_commands.json",
              file=sys.stderr)
        return 1

    compiler = None
    for entry in commands[:10]:
        toks = entry["command"].split()
        if toks and "riscv" in toks[0]:
            compiler = toks[0]
            break
    if compiler is None:
        compiler = "riscv32-esp-elf-gcc"

    sources = [
        os.path.join(src_dir, "src", "testapp_native.c"),
        os.path.join(src_dir, "src", "testapp_native_entry.c"),
    ]
    for s in sources:
        if not os.path.exists(s):
            print(f"ERROR: source file not found: {s}", file=sys.stderr)
            return 1

    out_dir = os.path.join(build_dir, "apps", "testapp_native")
    os.makedirs(out_dir, exist_ok=True)
    out_so = os.path.join(out_dir, "testapp_native.bin")

    own_includes = [
        f"-I{os.path.join(src_dir, 'include')}",
        f"-I{os.path.join(os.path.dirname(src_dir), '..', 'components', 'jpp_core', 'include')}",
    ]

    compile_flags = [
        "-c", "-fPIC", "-mno-relax",
        "-Os", "-g",
        "-Wall", "-Wno-unused-parameter",
        "-Wno-strict-prototypes",
        "-DESP_PLATFORM",
        "-D__riscv",
        "-DIDF_VER_MAJOR=5",
        "-DCONFIG_LOG_DEFAULT_LEVEL=3",
    ]

    obj_files: list[str] = []
    for src in sources:
        obj = os.path.join(out_dir, os.path.basename(src) + ".o")
        cmd = ([compiler] + compile_flags + own_includes + ref_flags
               + [src, "-o", obj])
        print(f"  CC {os.path.basename(src)}")
        result = subprocess.run(cmd)
        if result.returncode != 0:
            print(f"ERROR: compile failed for {src}", file=sys.stderr)
            return result.returncode
        obj_files.append(obj)

    print(f"  LD  testapp_native.bin")
    cmd = [compiler, "-shared", "-mno-relax", "-nostdlib",
           "-Wl,--allow-shlib-undefined"] + obj_files + ["-o", out_so]
    result = subprocess.run(cmd)
    if result.returncode != 0:
        print(f"ERROR: link failed (exit {result.returncode})", file=sys.stderr)
        return result.returncode

    # Remove intermediate object files: out_dir is a deploy directory copied
    # verbatim to /sd/apps/<id>/, so it must hold only the linked .bin (+
    # manifest), never compiler leftovers.
    for obj in obj_files:
        try:
            os.remove(obj)
        except OSError:
            pass

    manifest_src = os.path.join(src_dir, "manifest.json")
    manifest_dst = os.path.join(out_dir, "manifest.json")
    if os.path.exists(manifest_src):
        shutil.copy2(manifest_src, manifest_dst)
        print("  CP  manifest.json")

    size = os.path.getsize(out_so)
    print(f"OK  {out_so}  ({size // 1024} KB)")
    print()
    print("To deploy: copy testapp_native.so (as testapp_native.bin) and manifest.json")
    print("           to /sd/apps/testapp_native/ on the SD card.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <build_dir> <testapp_native_src_dir>", file=sys.stderr)
        sys.exit(1)
    sys.exit(main(sys.argv[1], sys.argv[2]))
