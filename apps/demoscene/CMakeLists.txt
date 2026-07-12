# ---------------------------------------------------------------------------
# DemoScene — native oldskool demo app for the J++Device.
#
# The ESP-IDF component registration below is kept for IDE support (include
# paths, code navigation, static analysis) but this component is NOT linked
# into the firmware. The firmware loads it at runtime from the SD card using
# jpp_native_loader_core.
#
# To build the loadable binary, run (from the project root, inside the Docker
# build environment):
#
#   idf.py build            # also triggers the demoscene_bin custom target
#
# The output is build/apps/demoscene/demoscene.bin (ELF32 shared object)
# and build/apps/demoscene/manifest.json — copy both to
# /sd/apps/demoscene/ on the SD card.
# ---------------------------------------------------------------------------

idf_component_register(
    SRCS
        "src/demoscene.c"
        "src/demoscene_entry.c"
    INCLUDE_DIRS "include"
    REQUIRES jpp_core freertos
)

# ---------------------------------------------------------------------------
# Standalone shared-library target (demoscene.bin).
# ---------------------------------------------------------------------------

set(_DEMOSCENE_BIN ${CMAKE_BINARY_DIR}/apps/demoscene/demoscene.bin)

idf_build_get_property(_demoscene_python PYTHON)

add_custom_target(demoscene_bin ALL
    COMMAND ${_demoscene_python}
            ${CMAKE_CURRENT_SOURCE_DIR}/build_shared.py
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building DemoScene shared library → ${_DEMOSCENE_BIN}"
    DEPENDS gen_project_binary
    VERBATIM
)
