#!/usr/bin/env python3
"""
Build MeetApp as a position-independent ELF32 shared library.

Reads build/compile_commands.json to obtain the exact -I/-D flags that ESP-IDF
uses for jpp_core compilation, then recompiles the MeetApp sources with those
flags plus -fPIC -shared -mno-relax, producing build/apps/meetapp/meetapp.so.

Invoked by the 'meetapp_bin' CMake custom target inside idf.py build, or
directly:  python3 apps/meetapp/build_shared.py <build_dir> <src_dir>
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

    # Extract -I flags from a jpp_core or jpp_sdk_bridge compilation.
    # We intentionally skip -D flags from the reference: jpp_core pulls in mbedtls
    # and wifi configuration defines that cause errors when the corresponding
    # include paths aren't present. We supply our own minimal -D set below.
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

    # Find the C compiler (same one ESP-IDF uses).
    compiler = None
    for entry in commands[:10]:
        toks = entry["command"].split()
        if toks and "riscv" in toks[0]:
            compiler = toks[0]
            break
    if compiler is None:
        compiler = "riscv32-esp-elf-gcc"

    # Sources relative to the meetapp src/ directory.
    sources = [
        os.path.join(src_dir, "src", "meetapp.c"),
        os.path.join(src_dir, "src", "meetapp_entry.c"),
        os.path.join(src_dir, "src", "meetapp_identity.c"),
        os.path.join(src_dir, "src", "meetapp_ble.c"),
        os.path.join(src_dir, "src", "meetapp_proof.c"),
    ]
    for s in sources:
        if not os.path.exists(s):
            print(f"ERROR: source file not found: {s}", file=sys.stderr)
            return 1

    out_dir = os.path.join(build_dir, "apps", "meetapp")
    os.makedirs(out_dir, exist_ok=True)
    out_so = os.path.join(out_dir, "meetapp.bin")

    # Add our own include directories.
    own_includes = [
        f"-I{os.path.join(src_dir, 'include')}",
        f"-I{os.path.join(os.path.dirname(src_dir), '..', 'components', 'jpp_core', 'include')}",
        f"-I{os.path.join(os.path.dirname(src_dir), '..', 'components', 'jpp_crypto_core', 'include')}",
    ]

    compile_flags = [
        "-c", "-fPIC", "-mno-relax",
        "-Os", "-g",
        "-Wall", "-Wno-unused-parameter",
        "-Wno-strict-prototypes",
        # Minimal defines needed for ESP-IDF headers (no mbedtls/wifi specifics)
        "-DESP_PLATFORM",
        "-D__riscv",
        "-DIDF_VER_MAJOR=5",
        "-DCONFIG_LOG_DEFAULT_LEVEL=3",
    ]

    # Stage 1: compile each source to a position-independent object file.
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

    # Stage 2: link object files into a shared library.
    # All calls to firmware functions (jpp_sdk_*, esp_log_write, snprintf, etc.)
    # are intentionally undefined here — they will be resolved at runtime by
    # jpp_native_loader_core from the firmware's exported symbol table.
    # Do NOT use -Wl,--no-undefined: the whole point of a shared library is that
    # it can have undefined symbols resolved later.
    print(f"  LD  meetapp.bin")
    # -nostdlib: prevent non-PIC libnosys/libgcc/crt from being pulled in.
    # Every symbol those libs would supply (snprintf, __udivdi3, etc.) appears as
    # UNDEF in the .so and is resolved at load time by jpp_native_loader_core from
    # the firmware's exported symbol table (jpp_native_symtab.c).
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
    print("To deploy: copy meetapp.so (as meetapp.bin) and manifest.json")
    print("           to /sd/apps/meetapp/ on the SD card.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <build_dir> <meetapp_src_dir>", file=sys.stderr)
        sys.exit(1)
    sys.exit(main(sys.argv[1], sys.argv[2]))
