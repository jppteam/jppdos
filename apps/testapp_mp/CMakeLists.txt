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
# Requires an mpy-cross emitting bytecode ABI 6 (mpy v6.3) on PATH. PyPI has
# no 1.28.0 release; the rc emits the same v6.3:
#   pip install mpy-cross==1.28.0rc0.post2
# The project Dockerfile instead builds v1.28.0 from source.
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
