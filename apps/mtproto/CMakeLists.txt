# ---------------------------------------------------------------------------
# MTProto (skeleton) — native envelope-measurement app for the J++Device.
#
# Like the other apps under apps/, this component is registered for IDE support
# (include paths, navigation, static analysis) but is NOT linked into the
# firmware. The firmware loads mtproto.bin at runtime from the SD card via
# jpp_native_loader_core.
#
# The loadable binary (build/apps/mtproto/mtproto.bin, an ELF32 shared object)
# plus manifest.json is produced by the mtproto_bin custom target during
# `idf.py build`. Copy both to /sd/apps/mtproto/ on the SD card.
#
# Requires SDK v2 (net_connect + jpp_crypto_* primitives).
# ---------------------------------------------------------------------------

idf_component_register(
    SRCS
        "src/mtproto.c"
        "src/mtproto_entry.c"
    INCLUDE_DIRS "include"
    REQUIRES jpp_core jpp_crypto_core freertos
)

# ---------------------------------------------------------------------------
# Standalone shared-library target (mtproto.bin).
# ---------------------------------------------------------------------------

set(_MTPROTO_BIN ${CMAKE_BINARY_DIR}/apps/mtproto/mtproto.bin)

idf_build_get_property(_mtproto_python PYTHON)

add_custom_target(mtproto_bin ALL
    COMMAND ${_mtproto_python}
            ${CMAKE_CURRENT_SOURCE_DIR}/build_shared.py
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building MTProto shared library → ${_MTPROTO_BIN}"
    DEPENDS gen_project_binary
    VERBATIM
)
