# MicroPython ESP-IDF component for JPPDOS.
#
# This component wraps MicroPython 1.28.0 as a static library consumed by
# jpp_core's jpp_mp_runner and jpp_mp_sdk_module.
#
# VENDORING
# ---------
# MicroPython source is expected at components/micropython/micropython/ as a
# git submodule pinned to the v1.28.0 tag:
#
#   git submodule add https://github.com/micropython/micropython.git \
#       components/micropython/micropython
#   git -C components/micropython/micropython checkout v1.28.0
#   git submodule update --init --recursive components/micropython/micropython
#
# After adding the submodule, run `make -C micropython/mpy-cross` once to build
# the cross-compiler used to produce .mpy files for apps.

cmake_minimum_required(VERSION 3.16)

set(MP_DIR "${CMAKE_CURRENT_LIST_DIR}/micropython")

# Verify the submodule is present.
if(NOT EXISTS "${MP_DIR}/py/runtime.c")
    message(FATAL_ERROR
        "MicroPython source not found at ${MP_DIR}.\n"
        "Run: git submodule update --init --recursive components/micropython/micropython")
endif()

# ------------------------------------------------------------------
# Core MicroPython py/ sources (interpreter, GC, compiler, runtime).
# ------------------------------------------------------------------
set(MP_PY_SRCS
    ${MP_DIR}/py/argcheck.c
    ${MP_DIR}/py/asmbase.c
    ${MP_DIR}/py/bc.c
    ${MP_DIR}/py/binary.c
    ${MP_DIR}/py/builtinevex.c
    ${MP_DIR}/py/builtinhelp.c
    ${MP_DIR}/py/builtinimport.c
    ${MP_DIR}/py/compile.c
    ${MP_DIR}/py/emitbc.c
    ${MP_DIR}/py/emitcommon.c
    ${MP_DIR}/py/emitglue.c
    ${MP_DIR}/py/emitinlinerv32.c
    ${MP_DIR}/py/formatfloat.c
    ${MP_DIR}/py/frozenmod.c
    ${MP_DIR}/py/gc.c
    ${MP_DIR}/py/lexer.c
    ${MP_DIR}/py/malloc.c
    ${MP_DIR}/py/map.c
    ${MP_DIR}/py/modarray.c
    ${MP_DIR}/py/modbuiltins.c
    ${MP_DIR}/py/modcollections.c
    ${MP_DIR}/py/modgc.c
    ${MP_DIR}/py/modio.c
    ${MP_DIR}/py/modmath.c
    ${MP_DIR}/py/modmicropython.c
    ${MP_DIR}/py/modstruct.c
    ${MP_DIR}/py/modsys.c
    ${MP_DIR}/py/modthread.c
    ${MP_DIR}/py/mpprint.c
    ${MP_DIR}/py/mpstate.c
    ${MP_DIR}/py/mpz.c
    ${MP_DIR}/py/nativeglue.c
    ${MP_DIR}/py/nlr.c
    ${MP_DIR}/py/nlrsetjmp.c
    ${MP_DIR}/py/obj.c
    ${MP_DIR}/py/objarray.c
    ${MP_DIR}/py/objattrtuple.c
    ${MP_DIR}/py/objbool.c
    ${MP_DIR}/py/objboundmeth.c
    ${MP_DIR}/py/objcell.c
    ${MP_DIR}/py/objclosure.c
    ${MP_DIR}/py/objcomplex.c
    ${MP_DIR}/py/objdeque.c
    ${MP_DIR}/py/objdict.c
    ${MP_DIR}/py/objenumerate.c
    ${MP_DIR}/py/objexcept.c
    ${MP_DIR}/py/objfilter.c
    ${MP_DIR}/py/objfloat.c
    ${MP_DIR}/py/objfun.c
    ${MP_DIR}/py/objgenerator.c
    ${MP_DIR}/py/objgetitemiter.c
    ${MP_DIR}/py/objint.c
    ${MP_DIR}/py/objint_longlong.c
    ${MP_DIR}/py/objint_mpz.c
    ${MP_DIR}/py/objlist.c
    ${MP_DIR}/py/objmap.c
    ${MP_DIR}/py/objmodule.c
    ${MP_DIR}/py/objnamedtuple.c
    ${MP_DIR}/py/objnone.c
    ${MP_DIR}/py/objobject.c
    ${MP_DIR}/py/objpolyiter.c
    ${MP_DIR}/py/objproperty.c
    ${MP_DIR}/py/objrange.c
    ${MP_DIR}/py/objreversed.c
    ${MP_DIR}/py/objset.c
    ${MP_DIR}/py/objsingleton.c
    ${MP_DIR}/py/objslice.c
    ${MP_DIR}/py/objstr.c
    ${MP_DIR}/py/objstringio.c
    ${MP_DIR}/py/objstrunicode.c
    ${MP_DIR}/py/objtuple.c
    ${MP_DIR}/py/objtype.c
    ${MP_DIR}/py/objzip.c
    ${MP_DIR}/py/opmethods.c
    ${MP_DIR}/py/pairheap.c
    ${MP_DIR}/py/parse.c
    ${MP_DIR}/py/parsenum.c
    ${MP_DIR}/py/parsenumbase.c
    ${MP_DIR}/py/persistentcode.c
    ${MP_DIR}/py/pystack.c
    ${MP_DIR}/py/qstr.c
    ${MP_DIR}/py/reader.c
    ${MP_DIR}/py/repl.c
    ${MP_DIR}/py/ringbuf.c
    ${MP_DIR}/py/runtime.c
    ${MP_DIR}/py/runtime_utils.c
    ${MP_DIR}/py/scheduler.c
    ${MP_DIR}/py/scope.c
    ${MP_DIR}/py/sequence.c
    ${MP_DIR}/py/showbc.c
    ${MP_DIR}/py/smallint.c
    ${MP_DIR}/py/stackctrl.c
    ${MP_DIR}/py/cstack.c
    ${MP_DIR}/py/stream.c
    ${MP_DIR}/py/vm.c
    ${MP_DIR}/py/unicode.c
    ${MP_DIR}/py/vstr.c
    ${MP_DIR}/py/warning.c
    ${MP_DIR}/shared/runtime/gchelper_generic.c
    ${MP_DIR}/shared/runtime/pyexec.c
    ${MP_DIR}/shared/runtime/stdout_helpers.c
)

# ------------------------------------------------------------------
# Port-specific glue living in this component directory.
# ------------------------------------------------------------------
set(MP_PORT_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/mphalport.c"
)

idf_component_register(
    SRCS
        ${MP_PY_SRCS}
        ${MP_PORT_SRCS}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}"     # mpconfigport.h, mphalport.h
        "${MP_DIR}"                     # micropython/ root (for py/obj.h etc.)
    REQUIRES
        freertos
        esp_timer
        newlib
)

# Tell MicroPython where to find its own port config.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    MICROPY_PORT_CONFIG_FILE="mpconfigport.h"
)

# Suppress warnings from MicroPython's own source that we cannot control.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-implicit-fallthrough
)

# ------------------------------------------------------------------
# Generate genhdr/ files following the official py/mkrules.cmake pipeline.
#
# Pipeline summary:
#  1. makeqstrdefs.py pp  — preprocess sources with -DNO_QSTR to extract
#                           MP_QSTR_xxx references → qstr.i.last
#  2. makeqstrdefs.py split qstr  → per-source .qstr files
#  3. makeqstrdefs.py cat  qstr   → qstrdefs.collected.h
#  4. cat qstrdefs.h + port + collected | sed | cpp | sed
#                                        → qstrdefs.preprocessed.h
#  5. makeqstrdata.py             → qstrdefs.generated.h (QDEF0/QDEF1 table)
#  Parallel tracks for moduledefs.h and root_pointers.h.
# ------------------------------------------------------------------
set(MP_GENHDR_DIR "${CMAKE_CURRENT_BINARY_DIR}/genhdr")

set(MP_QSTR_I_LAST           "${MP_GENHDR_DIR}/qstr.i.last")
set(MP_QSTR_SPLIT_STAMP      "${MP_GENHDR_DIR}/qstr.split")
set(MP_QSTR_COLLECTED        "${MP_GENHDR_DIR}/qstrdefs.collected.h")
set(MP_QSTRDEFS_PREPROCESSED "${MP_GENHDR_DIR}/qstrdefs.preprocessed.h")
set(MP_QSTRDEFS_GENERATED    "${MP_GENHDR_DIR}/qstrdefs.generated.h")

set(MP_MOD_SPLIT_STAMP       "${MP_GENHDR_DIR}/module.split")
set(MP_MOD_COLLECTED         "${MP_GENHDR_DIR}/moduledefs.collected.h")
set(MP_MODULEDEFS_H          "${MP_GENHDR_DIR}/moduledefs.h")

set(MP_RP_SPLIT_STAMP        "${MP_GENHDR_DIR}/root_pointer.split")
set(MP_RP_COLLECTED          "${MP_GENHDR_DIR}/root_pointers.collected.h")
set(MP_ROOT_POINTERS_H       "${MP_GENHDR_DIR}/root_pointers.h")

# Port-specific qstr definitions — jppsdk module and runner qstrs.
set(MP_QSTRDEFS_PORT "${CMAKE_CURRENT_LIST_DIR}/qstrdefsport.h")

# All source files to scan.
set(MP_SOURCE_QSTR ${MP_PY_SRCS} ${MP_PORT_SRCS})

# -- Step 0: Generate mpversion.h (required by modsys.c and others) --
set(MP_MPVERSION_H "${MP_GENHDR_DIR}/mpversion.h")
add_custom_command(
    OUTPUT "${MP_MPVERSION_H}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MP_GENHDR_DIR}"
    COMMAND python3 "${MP_DIR}/py/makeversionhdr.py" "${MP_MPVERSION_H}"
    WORKING_DIRECTORY "${MP_DIR}"
    COMMENT "Generating mpversion.h"
    VERBATIM
)
add_custom_target(__micropython_mpversion DEPENDS "${MP_MPVERSION_H}")

# -- Step 1: Preprocess all sources (-DNO_QSTR skips the circular include) --
# $<LIST:TRANSFORM,...> expands INCLUDE_DIRECTORIES into -I flags at build time.
add_custom_command(
    OUTPUT "${MP_QSTR_I_LAST}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MP_GENHDR_DIR}"
    COMMAND python3 "${MP_DIR}/py/makeqstrdefs.py"
        pp "${CMAKE_C_COMPILER}" -E
        output "${MP_QSTR_I_LAST}"
        cflags
            -DNO_QSTR
            "-DMICROPY_PORT_CONFIG_FILE=mpconfigport.h"
            "$<LIST:TRANSFORM,$<TARGET_PROPERTY:${COMPONENT_LIB},INCLUDE_DIRECTORIES>,PREPEND,-I>"
        sources ${MP_SOURCE_QSTR}
    COMMAND_EXPAND_LISTS
    DEPENDS ${MP_SOURCE_QSTR} "${MP_MPVERSION_H}" "${CMAKE_CURRENT_LIST_DIR}/mpconfigport.h"
    COMMENT "Preprocessing MicroPython sources for qstr scanning"
    VERBATIM
)

# -- Step 2: Split extracted qstr/module/root_pointer references --
add_custom_command(
    OUTPUT "${MP_QSTR_SPLIT_STAMP}"
    COMMAND python3 "${MP_DIR}/py/makeqstrdefs.py"
        split qstr "${MP_QSTR_I_LAST}" "${MP_GENHDR_DIR}/qstr" _
    COMMAND ${CMAKE_COMMAND} -E touch "${MP_QSTR_SPLIT_STAMP}"
    DEPENDS "${MP_QSTR_I_LAST}"
    COMMENT "Splitting qstr references"
    VERBATIM
)

add_custom_command(
    OUTPUT "${MP_MOD_SPLIT_STAMP}"
    COMMAND python3 "${MP_DIR}/py/makeqstrdefs.py"
        split module "${MP_QSTR_I_LAST}" "${MP_GENHDR_DIR}/module" _
    COMMAND ${CMAKE_COMMAND} -E touch "${MP_MOD_SPLIT_STAMP}"
    DEPENDS "${MP_QSTR_I_LAST}"
    COMMENT "Splitting module registrations"
    VERBATIM
)

add_custom_command(
    OUTPUT "${MP_RP_SPLIT_STAMP}"
    COMMAND python3 "${MP_DIR}/py/makeqstrdefs.py"
        split root_pointer "${MP_QSTR_I_LAST}" "${MP_GENHDR_DIR}/root_pointer" _
    COMMAND ${CMAKE_COMMAND} -E touch "${MP_RP_SPLIT_STAMP}"
    DEPENDS "${MP_QSTR_I_LAST}"
    COMMENT "Splitting root pointers"
    VERBATIM
)

# -- Step 3: Collect split files --
add_custom_command(
    OUTPUT "${MP_QSTR_COLLECTED}"
    BYPRODUCTS "${MP_QSTR_COLLECTED}.hash"
    COMMAND python3 "${MP_DIR}/py/makeqstrdefs.py"
        cat qstr _ "${MP_GENHDR_DIR}/qstr" "${MP_QSTR_COLLECTED}"
    DEPENDS "${MP_QSTR_SPLIT_STAMP}"
    COMMENT "Collecting qstrs"
    VERBATIM
)

add_custom_command(
    OUTPUT "${MP_MOD_COLLECTED}"
    BYPRODUCTS "${MP_MOD_COLLECTED}.hash"
    COMMAND python3 "${MP_DIR}/py/makeqstrdefs.py"
        cat module _ "${MP_GENHDR_DIR}/module" "${MP_MOD_COLLECTED}"
    DEPENDS "${MP_MOD_SPLIT_STAMP}"
    COMMENT "Collecting module registrations"
    VERBATIM
)

add_custom_command(
    OUTPUT "${MP_RP_COLLECTED}"
    BYPRODUCTS "${MP_RP_COLLECTED}.hash"
    COMMAND python3 "${MP_DIR}/py/makeqstrdefs.py"
        cat root_pointer _ "${MP_GENHDR_DIR}/root_pointer" "${MP_RP_COLLECTED}"
    DEPENDS "${MP_RP_SPLIT_STAMP}"
    COMMENT "Collecting root pointers"
    VERBATIM
)

# -- Step 4: Preprocess the combined qstr definitions (sed | cpp | sed pipeline) --
add_custom_command(
    OUTPUT "${MP_QSTRDEFS_PREPROCESSED}"
    COMMAND /bin/sh -c
        "cat '${MP_DIR}/py/qstrdefs.h' '${MP_QSTRDEFS_PORT}' '${MP_QSTR_COLLECTED}' \
         | sed 's/^Q(.*/\"&\"/' \
         | ${CMAKE_C_COMPILER} -E -DMICROPY_PORT_CONFIG_FILE='mpconfigport.h' \
             -I '${CMAKE_CURRENT_LIST_DIR}' -I '${MP_DIR}' - \
         | sed 's/^\"\\(Q(.*)\\)\"/\\1/' \
         > '${MP_QSTRDEFS_PREPROCESSED}'"
    DEPENDS
        "${MP_DIR}/py/qstrdefs.h"
        "${MP_QSTRDEFS_PORT}"
        "${MP_QSTR_COLLECTED}"
    COMMENT "Preprocessing qstrdefs definitions"
    VERBATIM
)

# -- Step 5: Generate the final headers from collected data --
add_custom_command(
    OUTPUT "${MP_QSTRDEFS_GENERATED}"
    COMMAND python3 "${MP_DIR}/py/makeqstrdata.py" "${MP_QSTRDEFS_PREPROCESSED}"
        > "${MP_QSTRDEFS_GENERATED}"
    DEPENDS "${MP_QSTRDEFS_PREPROCESSED}"
    COMMENT "Generating qstrdefs.generated.h"
    VERBATIM
)

add_custom_command(
    OUTPUT "${MP_MODULEDEFS_H}"
    COMMAND python3 "${MP_DIR}/py/makemoduledefs.py" "${MP_MOD_COLLECTED}"
        > "${MP_MODULEDEFS_H}"
    DEPENDS "${MP_MOD_COLLECTED}"
    COMMENT "Generating moduledefs.h"
    VERBATIM
)

add_custom_command(
    OUTPUT "${MP_ROOT_POINTERS_H}"
    COMMAND python3 "${MP_DIR}/py/make_root_pointers.py" "${MP_RP_COLLECTED}"
        > "${MP_ROOT_POINTERS_H}"
    DEPENDS "${MP_RP_COLLECTED}"
    COMMENT "Generating root_pointers.h"
    VERBATIM
)

add_custom_target(__micropython_genhdr
    DEPENDS
        "${MP_MPVERSION_H}"
        "${MP_QSTRDEFS_GENERATED}"
        "${MP_MODULEDEFS_H}"
        "${MP_ROOT_POINTERS_H}"
)

# Expose the build dir so #include "genhdr/..." resolves — PUBLIC so
# components that include MicroPython headers (jpp_mp_runner, jpp_mp_sdk_module)
# also get the genhdr directory in their include path.
target_include_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(${COMPONENT_LIB} __micropython_genhdr)
