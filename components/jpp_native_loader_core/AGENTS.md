# jpp_native_loader_core

ELF32/RISC-V position-independent app loader. Loads native app binaries
(`app_type: "native"`) from the SD card at runtime without static linking.

## How it works

A native app binary is an ELF32 shared object (`ET_DYN`) compiled for
`riscv32-esp-elf` with `-fPIC -shared -mno-relax -nostartfiles`. The loader:

1. Reads and validates the ELF header (RISC-V, 32-bit, little-endian, ET_DYN).
2. Acquires the shared `jpp_app_pool` (64 KB static `.bss`, executable on the C6
   because `CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n` maps all SRAM RWX) covering all
   PT_LOAD segments. The pool is shared with the MicroPython GC heap — native and
   MP apps are mutually exclusive, so it is always free at launch.
3. Copies each PT_LOAD segment from the file; zero-fills the BSS tail.
4. Parses PT_DYNAMIC to locate the relocation tables (`.rela.dyn`, `.rela.plt`)
   and the dynamic symbol table.
5. Applies relocations:
   - `R_RISCV_RELATIVE` — internal data pointers (no symbol lookup).
   - `R_RISCV_JUMP_SLOT` — external function PLT/GOT slots; resolved from the
     firmware export table in `jpp_native_symtab.c`.
   - `R_RISCV_32` — absolute 32-bit refs, resolved likewise.
6. Finds `jpp_app_entry` in `.dynsym` and returns a handle.
7. Flushes the instruction cache (`__builtin___clear_cache`).

`jpp_native_loader_run(app, ctx)` calls `jpp_app_entry(ctx)` and blocks.
`jpp_native_loader_free(app)` releases the shared app pool and frees the handle.

## Symbol table (`jpp_native_symtab.c`)

Every external symbol an app may call through its PLT must be present. The
table currently covers the full `jpp_sdk_*` surface, `jpp_broker_result_*`,
`jpp_crypto_*`, `randombytes_buf`, `esp_log_write`, key FreeRTOS functions, and
the newlib standard C library.

**If a new app is rejected at boot with `UNRESOLVED_SYM`:** add the missing
symbol to `s_symtab` in `jpp_native_symtab.c`. No other code changes are needed.

## App build requirements

Compile the app sources with the `riscv32-esp-elf` toolchain:

```bash
riscv32-esp-elf-gcc \
  -shared -fPIC -mno-relax -nostartfiles \
  -I <jpp_sdk_bridge.h parent> \
  ... source files ... \
  -o app.so
```

The binary must export `jpp_app_entry(jpp_sdk_context_t *)` as a global symbol.
The resulting `.so` file (which is a valid ELF) is placed on the SD card as
`/sd/apps/<app_id>/<entry>` (matching `manifest.json` `entry` field).

## Error codes

| Code | Meaning |
|---|---|
| `OK` | Loaded successfully |
| `READ_FAILED` | File not found or I/O error |
| `INVALID_ELF` | Not a valid ELF32 RISC-V shared object |
| `UNSUPPORTED` | Relocation type not handled |
| `NO_MEMORY` | shared `jpp_app_pool` busy, or binary exceeds the 64 KB pool |
| `UNRESOLVED_SYM` | Symbol missing from firmware export table |
| `NO_ENTRY` | `jpp_app_entry` not exported by the binary |
