/*
 * MicroPython port configuration for JPPDOS on ESP32-C6.
 *
 * Heap and stack sizes are not set here — jpp_mp_runner allocates the GC
 * heap dynamically from jpp_vm_runtime_config_t.memory at session start.
 */

#pragma once

#include <stdint.h>
#include <alloca.h>

/* ------------------------------------------------------------------
 * Core feature set
 * ------------------------------------------------------------------ */
#define MICROPY_ENABLE_COMPILER         (1)
#define MICROPY_ENABLE_GC               (1)
#define MICROPY_HELPER_REPL             (1)     /* needed by pyexec.c friendly-REPL path */
#define MICROPY_HELPER_LEXER_UNIX       (0)
#define MICROPY_ENABLE_SOURCE_LINE      (1)     /* better tracebacks */
#define MICROPY_ENABLE_DOC_STRING       (0)     /* save flash */
#define MICROPY_ERROR_REPORTING         MICROPY_ERROR_REPORTING_TERSE
#define MICROPY_WARNINGS                (0)

/* ------------------------------------------------------------------
 * Integer / float
 * ------------------------------------------------------------------ */
#define MICROPY_LONGINT_IMPL            MICROPY_LONGINT_IMPL_MPZ
#define MICROPY_FLOAT_IMPL              MICROPY_FLOAT_IMPL_FLOAT

/* ------------------------------------------------------------------
 * Modules — only enable what apps are allowed to use.
 * Blocked modules (machine, network, socket, os, esp, esp32) must NOT
 * be enabled here; the import policy in jpp_vm_core provides the
 * second line of defence, but not enabling them at all is safer.
 * ------------------------------------------------------------------ */
#define MICROPY_PY_ARRAY                (1)
#define MICROPY_PY_COLLECTIONS          (1)
#define MICROPY_PY_IO                   (1)
#define MICROPY_PY_STRUCT               (1)
#define MICROPY_PY_SYS                  (1)
#define MICROPY_PY_SYS_EXIT             (1)
#define MICROPY_PY_MATH                 (1)
#define MICROPY_PY_MICROPYTHON          (1)
#define MICROPY_PY_GC                   (1)
#define MICROPY_PY_BUILTINS_BYTEARRAY   (1)
#define MICROPY_PY_BUILTINS_MEMORYVIEW  (1)
#define MICROPY_PY_BUILTINS_ENUMERATE   (1)
#define MICROPY_PY_BUILTINS_FILTER      (1)
#define MICROPY_PY_BUILTINS_MAP         (1)
#define MICROPY_PY_BUILTINS_REVERSED    (1)
#define MICROPY_PY_BUILTINS_SET         (1)
#define MICROPY_PY_BUILTINS_SLICE       (1)
#define MICROPY_PY_BUILTINS_PROPERTY    (1)
#define MICROPY_PY_BUILTINS_STR_UNICODE (1)

/* Disable privileged / hardware-touching built-in modules. */
#define MICROPY_PY_MACHINE              (0)
#define MICROPY_PY_NETWORK              (0)
#define MICROPY_PY_SOCKET               (0)
#define MICROPY_PY_USSL                 (0)
#define MICROPY_PY_THREAD               (0)  /* single-threaded per app */

/* ------------------------------------------------------------------
 * Memory layout
 * ------------------------------------------------------------------ */
#define MICROPY_STACK_CHECK             (1)
#define MICROPY_ENABLE_PYSTACK          (0)
#define MICROPY_ALLOC_PATH_MAX          (128)

/* ------------------------------------------------------------------
 * Type aliases required by MicroPython internals
 * ------------------------------------------------------------------ */
typedef int mp_int_t;
typedef unsigned mp_uint_t;
typedef long mp_off_t;

#define MP_SSIZE_MAX INT_MAX

/* ------------------------------------------------------------------
 * ROM string table
 * ------------------------------------------------------------------ */
#define MICROPY_QSTR_BYTES_IN_HASH      (1)
#define MICROPY_ALLOC_QSTR_CHUNK_INIT   (32)

/* ------------------------------------------------------------------
 * Bytecode loader — required for .mpy file support
 * ------------------------------------------------------------------ */
#define MICROPY_PERSISTENT_CODE_LOAD    (1)
#define MICROPY_PERSISTENT_CODE_SAVE    (0)

/* Enable the POSIX file reader so MicroPython can open .mpy files from the
 * filesystem.  This causes mpconfig.h to set MICROPY_HAS_FILE_READER=1
 * (derived as MICROPY_READER_POSIX || MICROPY_READER_VFS), which gates the
 * mp_raw_code_load_file() call in builtinimport.c.  Without this flag the
 * entire .mpy loading path is compiled out and every import falls through to
 * mp_lexer_new_from_file(), which raises OSError on this port. */
#define MICROPY_READER_POSIX            (1)

/* ------------------------------------------------------------------
 * NLR (non-local return) — use the setjmp variant on RISC-V
 * ------------------------------------------------------------------ */
#define MICROPY_NLR_SETJMP              (1)

/* ------------------------------------------------------------------
 * Platform identification (used by the sys module)
 * ------------------------------------------------------------------ */
#define MICROPY_PY_SYS_PLATFORM         "esp32"

/* ------------------------------------------------------------------
 * Port-provided I/O
 * ------------------------------------------------------------------ */
#define MICROPY_HAL_HAS_VT100           (0)

extern void mp_hal_stdout_tx_str(const char *str);
#define MICROPY_HAL_STDOUT_TX_STR       mp_hal_stdout_tx_str

extern int mp_hal_stdin_rx_chr(void);
#define MICROPY_HAL_STDIN_RX_CHR        mp_hal_stdin_rx_chr
