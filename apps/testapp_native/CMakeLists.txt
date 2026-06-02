# ---------------------------------------------------------------------------
# TestAppNative — native binary app for the J++Device.
#
# The ESP-IDF component registration below is kept for IDE support (include
# paths, code navigation, static analysis) but this component is NOT linked
# into the firmware. The firmware loads it at runtime from the SD card using
# jpp_native_loader_core.
#
# To build the loadable binary, run (from the project root, inside the Docker
# build environment):
#
#   idf.py build            # also triggers the testapp_native_bin custom target
#
# The output is build/apps/testapp_native/testapp_native.bin (ELF32 shared object)
# and build/apps/testapp_native/manifest.json — copy both to
# /sd/apps/testapp_native/ on the SD card.
# ---------------------------------------------------------------------------

idf_component_register(
    SRCS
        "src/testapp_native.c"
        "src/testapp_native_entry.c"
    INCLUDE_DIRS "include"
    REQUIRES jpp_core freertos
)

# ---------------------------------------------------------------------------
# Standalone shared-library target (testapp_native.bin).
# ---------------------------------------------------------------------------

set(_TESTAPP_BIN ${CMAKE_BINARY_DIR}/apps/testapp_native/testapp_native.bin)

idf_build_get_property(_testapp_python PYTHON)

add_custom_target(testapp_native_bin ALL
    COMMAND ${_testapp_python}
            ${CMAKE_CURRENT_SOURCE_DIR}/build_shared.py
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building TestAppNative shared library → ${_TESTAPP_BIN}"
    DEPENDS gen_project_binary
    VERBATIM
)
