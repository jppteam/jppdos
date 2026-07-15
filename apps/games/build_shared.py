#!/usr/bin/env python3
"""
Build the Games hub and its game modules as position-independent ELF32 shared
libraries.

Reads build/compile_commands.json to obtain the exact -I flags that ESP-IDF
uses for jpp_core compilation, then recompiles the sources with those flags
plus -fPIC -shared -mno-relax, producing:

  build/apps/games/games.bin          — the hub (jpp_app_entry)
  build/apps/games/<name>.mod.bin     — one per game (jpp_module_entry)
  build/apps/games/manifest.json

Invoked by the 'games_bin' CMake custom target inside idf.py build, or
directly:  python3 apps/games/build_shared.py <build_dir> <src_dir>
"""

import json
import os
import shutil
import subprocess
import sys

HUB_SOURCES = [
    "hub.c",
    "games_entry.c",
    "games_gfx.c",
    "games_store.c",
    "games_sfx.c",
    "games_rng.c",
    "games_ble.c",
]

# Each module is a single translation unit exporting jpp_module_entry.
MODULES = [
    "tetris",
    "pong",
    "snake",
    "breakout",
    "g2048",
    "flappy",
    "racer",
    "connect4",
    "battleship",
]


def find_toolchain(build_dir: str):
    cc_path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(cc_path):
        print(f"ERROR: {cc_path} not found; run idf.py build first", file=sys.stderr)
        return None, None

    with open(cc_path) as f:
        commands = json.load(f)

    ref_flags: list[str] = []
    for entry in commands:
        if "jpp_sdk_bridge" in entry["file"] or "jpp_core" in entry["file"]:
            for tok in entry["command"].split():
                if tok.startswith("-I") and tok not in ref_flags:
                    ref_flags.append(tok)
            break
    if not ref_flags:
        print("ERROR: could not find a jpp_core compile entry in compile_commands.json",
              file=sys.stderr)
        return None, None

    compiler = None
    for entry in commands[:10]:
        toks = entry["command"].split()
        if toks and "riscv" in toks[0]:
            compiler = toks[0]
            break
    if compiler is None:
        compiler = "riscv32-esp-elf-gcc"
    return compiler, ref_flags


def compile_and_link(compiler, compile_flags, includes, ref_flags,
                     sources, out_dir, out_bin) -> bool:
    obj_files = []
    for src in sources:
        obj = os.path.join(out_dir, os.path.basename(src) + ".o")
        cmd = ([compiler] + compile_flags + includes + ref_flags + [src, "-o", obj])
        print(f"  CC {os.path.basename(src)}")
        if subprocess.run(cmd).returncode != 0:
            print(f"ERROR: compile failed for {src}", file=sys.stderr)
            return False
        obj_files.append(obj)

    print(f"  LD  {os.path.basename(out_bin)}")
    cmd = [compiler, "-shared", "-mno-relax", "-nostdlib",
           "-Wl,--allow-shlib-undefined"] + obj_files + ["-o", out_bin]
    if subprocess.run(cmd).returncode != 0:
        print(f"ERROR: link failed for {out_bin}", file=sys.stderr)
        return False
    # Remove the intermediate object files: out_dir is a deploy directory whose
    # contents are copied verbatim to /sd/apps/<id>/, so it must contain only
    # the linked .bin (+ manifest), never compiler leftovers.
    for obj in obj_files:
        try:
            os.remove(obj)
        except OSError:
            pass
    return True


def main(build_dir: str, src_dir: str) -> int:
    compiler, ref_flags = find_toolchain(build_dir)
    if compiler is None:
        return 1

    out_dir = os.path.join(build_dir, "apps", "games")
    os.makedirs(out_dir, exist_ok=True)

    includes = [
        f"-I{os.path.join(src_dir, 'include')}",
        f"-I{os.path.join(src_dir, 'src')}",
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

    # Hub.
    hub_sources = [os.path.join(src_dir, "src", s) for s in HUB_SOURCES]
    for s in hub_sources:
        if not os.path.exists(s):
            print(f"ERROR: source file not found: {s}", file=sys.stderr)
            return 1
    hub_bin = os.path.join(out_dir, "games.bin")
    if not compile_and_link(compiler, compile_flags, includes, ref_flags,
                            hub_sources, out_dir, hub_bin):
        return 1

    # Game modules — one ELF each.
    total_mod = 0
    for name in MODULES:
        src = os.path.join(src_dir, "src", "mod", f"{name}.c")
        if not os.path.exists(src):
            print(f"ERROR: module source not found: {src}", file=sys.stderr)
            return 1
        mod_bin = os.path.join(out_dir, f"{name}.mod.bin")
        if not compile_and_link(compiler, compile_flags, includes, ref_flags,
                                [src], out_dir, mod_bin):
            return 1
        total_mod += os.path.getsize(mod_bin)

    manifest_src = os.path.join(src_dir, "manifest.json")
    if os.path.exists(manifest_src):
        shutil.copy2(manifest_src, os.path.join(out_dir, "manifest.json"))
        print("  CP  manifest.json")

    hub_size = os.path.getsize(hub_bin)
    print(f"OK  {hub_bin}  (hub {hub_size // 1024} KB, "
          f"{len(MODULES)} modules {total_mod // 1024} KB total)")
    print()
    print("To deploy: copy the contents of build/apps/games/ to /sd/apps/games/")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <build_dir> <games_src_dir>", file=sys.stderr)
        sys.exit(1)
    sys.exit(main(sys.argv[1], sys.argv[2]))
