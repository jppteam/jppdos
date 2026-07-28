# ---------------------------------------------------------------------------
# TestAppMP — MicroPython app for the J++Device.
#
# No C sources: this component registration exists for IDE support and to wire
# in the testapp_mp_bin custom target. The target compiles main.py to main.mpy
# with mpy-cross and stages both artefacts to build/apps/testapp_mp/.
#
# To build (from the project root, inside the Docker build environment):
#
#   idf.py build     # also triggers testapp_mp_bin
#
# Requires mpy-cross 1.28.0 on PATH:
#   pip install mpy-cross==1.28.0
#
# Output:
#   build/apps/testapp_mp/main.mpy
#   build/apps/testapp_mp/manifest.json
# ---------------------------------------------------------------------------

idf_component_register()

idf_build_get_property(_testapp_mp_python PYTHON)

add_custom_target(testapp_mp_bin ALL
    COMMAND ${_testapp_mp_python}
            ${CMAKE_CURRENT_SOURCE_DIR}/build_mp.py
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building TestAppMP bytecode → ${CMAKE_BINARY_DIR}/apps/testapp_mp/main.mpy"
    VERBATIM
)
