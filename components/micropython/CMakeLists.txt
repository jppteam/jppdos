# MicroPython ESP-IDF component for JPPDOS.
#
# This component wraps MicroPython as a static library consumed by jpp_core's
# jpp_mp_runner and jpp_mp_sdk_module.
#
# SOURCING
# --------
# The MicroPython source is FETCHED AT CMAKE CONFIGURE TIME, not vendored. It
# used to be a git submodule at components/micropython/micropython/, which cost
# 1.6 GB of checkout (1.5 GB of it MicroPython's own lib/ submodules) and 2.2 GB
# of .git/modules to deliver the ~3.4 MB this component actually compiles: py/
# and shared/runtime/. Nothing under lib/, extmod/, or ports/ is referenced, so
# a source archive — which by definition contains no submodules — carries
# everything needed in 8 MB, and `idf.py build` obtains it with no `git
# submodule update` step to forget.
#
# The pin is an exact commit plus its SHA-256, so this is every bit as
# reproducible as the submodule was: MP_COMMIT is the same revision the
# submodule pointed at, and the build fails loudly on any content mismatch
# rather than silently compiling something else.
#
# To move MicroPython: change MP_COMMIT, then update MP_ARCHIVE_SHA256 (the
# build prints the expected/actual pair on mismatch) and MP_GIT_TAG/MP_GIT_HASH
# to what `py/makeversionhdr.py` would derive at that commit. Keep mpy-cross in
# step — apps ship .mpy bytecode, and its version must match this interpreter
# (see AGENTS.md; the project Dockerfile builds mpy-cross).

cmake_minimum_required(VERSION 3.16)

include(FetchContent)

# The pinned MicroPython revision. NOTE: this is a v1.29.0-preview commit, not
# the v1.28.0 release — it is exactly what the submodule pointed at, carried
# over unchanged so this switch is a no-op for the resulting binary.
set(MP_COMMIT "1c63211817d9c5164542b94771634cf80b300fdf")
set(MP_ARCHIVE_SHA256
    "0fbc545074a0960fc6c64c161bcc883aa101889514eb99f2d52a2ed9cefdf18e")

# What py/makeversionhdr.py derives from git describe at MP_COMMIT. An archive
# has no .git, so these are passed to it via the MICROPY_GIT_TAG/_HASH env vars
# it documents for exactly this case (see "Step 0" below), keeping the generated
# mpversion.h identical to what the submodule build produced.
set(MP_GIT_TAG  "v1.29.0-preview.245.g1c63211817")
set(MP_GIT_HASH "1c6321181")

# Fetched sources live OUTSIDE the build directory, keyed by commit, so that
# `idf.py fullclean` does not throw the download away and a pin change does not
# overwrite the previous tree. Override to point at a local MicroPython checkout
# (for example when bisecting upstream):
#     idf.py -DMP_SOURCE_DIR=/path/to/micropython build
set(MP_FETCH_ROOT "${CMAKE_SOURCE_DIR}/.deps/micropython-${MP_COMMIT}"
    CACHE PATH "Where the pinned MicroPython source archive is unpacked")
set(MP_SOURCE_DIR "" CACHE PATH
    "Use an existing MicroPython checkout instead of fetching the pinned one")

if(MP_SOURCE_DIR)
    set(MP_DIR "${MP_SOURCE_DIR}")
    message(STATUS "MicroPython: using local checkout at ${MP_DIR}")
elseif(CMAKE_SCRIPT_MODE_FILE)
    # ESP-IDF expands component requirements in an early pass that re-runs this
    # file with `cmake -P` (tools/cmake/scripts/component_get_requirements.cmake).
    # FetchContent cannot run there: FetchContent_Declare reaches define_property,
    # which script mode does not provide, and the whole configure aborts. That
    # pass reads only the REQUIRES list — idf_component_register ignores SRCS —
    # so there is nothing to fetch for it. Point MP_DIR at where the real
    # configure pass unpacks the archive and skip the download.
    set(MP_DIR "${MP_FETCH_ROOT}/src")
else()
    # No CMakeLists.txt at the MicroPython repo root, so MakeAvailable populates
    # the source without trying to add_subdirectory() it into the IDF build.
    FetchContent_Declare(micropython_src
        URL      "https://github.com/micropython/micropython/archive/${MP_COMMIT}.tar.gz"
        URL_HASH "SHA256=${MP_ARCHIVE_SHA256}"
        SOURCE_DIR   "${MP_FETCH_ROOT}/src"
        SUBBUILD_DIR "${MP_FETCH_ROOT}/subbuild"
        BINARY_DIR   "${MP_FETCH_ROOT}/build"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(micropython_src)
    set(MP_DIR "${micropython_src_SOURCE_DIR}")
    message(STATUS "MicroPython: ${MP_GIT_TAG} at ${MP_DIR}")
endif()

# Not checked in script mode: the tree legitimately does not exist yet during
# requirements expansion, and that pass compiles nothing.
if(NOT CMAKE_SCRIPT_MODE_FILE AND NOT EXISTS "${MP_DIR}/py/runtime.c")
    message(FATAL_ERROR
        "MicroPython source incomplete at ${MP_DIR} — py/runtime.c is missing.\n"
        "Delete ${MP_FETCH_ROOT} and re-run the build to refetch.")
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
    # The fetched tree is an archive with no .git, so makeversionhdr.py cannot
    # run `git describe`. Its documented MICROPY_GIT_TAG/MICROPY_GIT_HASH env
    # overrides supply the same values that describe produced at MP_COMMIT —
    # without them it would silently fall back to mpconfig.h and emit a vaguer
    # "v1.29.0-preview" with no hash.
    COMMAND ${CMAKE_COMMAND} -E env
        "MICROPY_GIT_TAG=${MP_GIT_TAG}"
        "MICROPY_GIT_HASH=${MP_GIT_HASH}"
        python3 "${MP_DIR}/py/makeversionhdr.py" "${MP_MPVERSION_H}"
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
