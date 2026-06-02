cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "apps")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ---------------------------------------------------------------------------
# Wokwi build configuration
#
# IMPORTANT: idf_build_set_property(COMPILE_DEFINITIONS) must be called
# BEFORE project() to reach all component libraries.  add_compile_definitions()
# after project() only propagates to the main executable glue file
# (project_elf_src_esp32c6.c), not to component targets such as jpp_core or
# the main component.
#
# Wokwi merged-flash image
#
# Wokwi only loads the single binary named by the "firmware" key in
# wokwi.toml.  It places that binary at 0x10000, so a bare app binary
# leaves flash addresses 0x0 (bootloader) and 0x8000 (partition table)
# empty.  Without the partition table the SPIFFS driver cannot find the
# "data_fs" / "runtime_fs" labels and panics at boot.
#
# Fix: after every build, merge bootloader + partition-table + app into
# build/merged.bin (a flat flash image starting at 0x0).  wokwi.toml
# points "firmware" at this file so Wokwi loads the full layout.
#
# Single-app embed mode (JPP_WOKWI_EMBED_APP_PATH):
#   idf.py -DJPP_WOKWI_EMBED_APP_PATH=/path/to/app build
#   The app directory must contain manifest.json plus pre-compiled .mpy files.
#   The app is packed into a SPIFFS image and baked into merged.bin at the
#   data_fs partition offset (0x210000).  It then appears in the launcher
#   at runtime and is launched like any SD app.
# ---------------------------------------------------------------------------
set(JPP_WOKWI_EMBED_APP_PATH "" CACHE STRING
    "Path to app dir to embed in data_fs SPIFFS (empty = normal build)")

# ---------------------------------------------------------------------------
# Wokwi keypad simulation shim (JPP_WOKWI_SIM)
#
# Wokwi cannot model the analog resistor-ladder keypad, so the firmware carries
# a digital push-button shim on dedicated GPIOs (see main/jpp_hw_config.h). That
# shim is SIMULATION ONLY: a real-hardware build must NOT compile it in, or it
# would drive GPIOs that don't belong to the keypad on the physical board.
# JPP_WOKWI_SIM also disables the NimBLE stack, whose controller emulation in
# Wokwi is incomplete and causes an interrupt WDT crash at boot.
#
# Enable it explicitly for Wokwi images:
#   idf.py -DJPP_WOKWI_SIM=ON build
# Embedding an app (JPP_WOKWI_EMBED_APP_PATH) is by definition a Wokwi build, so
# it turns the shim on automatically below.
# ---------------------------------------------------------------------------
option(JPP_WOKWI_SIM "Compile in the Wokwi-only GPIO keypad shim" OFF)
if(NOT "${JPP_WOKWI_EMBED_APP_PATH}" STREQUAL "")
    set(JPP_WOKWI_SIM ON)
    if(NOT IS_ABSOLUTE "${JPP_WOKWI_EMBED_APP_PATH}")
        get_filename_component(JPP_WOKWI_EMBED_APP_PATH
            "${CMAKE_SOURCE_DIR}/${JPP_WOKWI_EMBED_APP_PATH}" ABSOLUTE)
    endif()
endif()
if(JPP_WOKWI_SIM)
    idf_build_set_property(COMPILE_DEFINITIONS "JPP_WOKWI_SIM=1" APPEND)
    message(STATUS "JPP_WOKWI_SIM enabled — Wokwi keypad shim compiled in")
endif()

idf_build_get_property(python PYTHON)

if(NOT "${JPP_WOKWI_EMBED_APP_PATH}" STREQUAL "")

    if(NOT EXISTS "${JPP_WOKWI_EMBED_APP_PATH}/manifest.json")
        message(FATAL_ERROR
            "JPP_WOKWI_EMBED_APP_PATH='${JPP_WOKWI_EMBED_APP_PATH}': manifest.json not found")
    endif()

    execute_process(
        COMMAND "${python}"
            -c "import json,sys; d=json.load(open(sys.argv[1])); print(d['app_id'],end='')"
            "${JPP_WOKWI_EMBED_APP_PATH}/manifest.json"
        OUTPUT_VARIABLE JPP_EMBED_APP_ID
        RESULT_VARIABLE _app_id_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _app_id_rc EQUAL 0 OR "${JPP_EMBED_APP_ID}" STREQUAL "")
        message(FATAL_ERROR "Failed to extract app_id from manifest.json")
    endif()
    message(STATUS "JPP_WOKWI embed: app_id='${JPP_EMBED_APP_ID}' src='${JPP_WOKWI_EMBED_APP_PATH}'")

    set(_staging_root "${CMAKE_BINARY_DIR}/wokwi_embed_staging")
    set(_staging_app  "${_staging_root}/apps/${JPP_EMBED_APP_ID}")
    file(REMOVE_RECURSE "${_staging_root}")
    file(MAKE_DIRECTORY "${_staging_app}")

    # Copy only app data files (manifest + Python bytecode) — never C sources.
    # SPIFFS has a 32-character path limit; C source paths easily exceed it.
    # Preserve relative paths so "main.mpy" at app root lands at
    # staging/apps/<id>/main.mpy, and "python/foo.mpy" lands under python/.
    file(COPY "${JPP_WOKWI_EMBED_APP_PATH}/manifest.json"
         DESTINATION "${_staging_app}")
    file(GLOB_RECURSE _mpy_files
         "${JPP_WOKWI_EMBED_APP_PATH}/*.mpy"
         "${JPP_WOKWI_EMBED_APP_PATH}/*.py")
    foreach(_f ${_mpy_files})
        file(RELATIVE_PATH _rel "${JPP_WOKWI_EMBED_APP_PATH}" "${_f}")
        get_filename_component(_reldir "${_rel}" DIRECTORY)
        if(_reldir)
            file(COPY "${_f}" DESTINATION "${_staging_app}/${_reldir}/")
        else()
            file(COPY "${_f}" DESTINATION "${_staging_app}/")
        endif()
    endforeach()

    idf_build_set_property(COMPILE_DEFINITIONS "JPP_WOKWI_EMBED_APP=1" APPEND)
    idf_build_set_property(COMPILE_DEFINITIONS "JPP_WOKWI_EMBED_APP_ID=\"${JPP_EMBED_APP_ID}\"" APPEND)

endif()

project(jppdos_idf)

# ---------------------------------------------------------------------------
# Post-build: produce build/merged.bin
# add_custom_command must come after project() (needs gen_project_binary target).
# ---------------------------------------------------------------------------

if(NOT "${JPP_WOKWI_EMBED_APP_PATH}" STREQUAL "")

    # POST_BUILD 1: generate SPIFFS image (must run before merge).
    # 262144 = 0x40000 = data_fs partition size in partitions.csv.
    # CONFIG_SPIFFS_USE_MAGIC is disabled in sdkconfig so the driver mounts
    # a pre-generated image without requiring magic bytes at block boundaries.
    add_custom_command(
        TARGET gen_project_binary POST_BUILD
        COMMAND "${python}" "$ENV{IDF_PATH}/components/spiffs/spiffsgen.py"
                262144
                "${_staging_root}"
                "${CMAKE_BINARY_DIR}/wokwi_embed_data_fs.bin"
                --page-size 256
                --obj-name-len 32
                --meta-len 4
        COMMENT "Generating SPIFFS image → wokwi_embed_data_fs.bin"
        VERBATIM
    )

    # POST_BUILD 2: merge including embedded SPIFFS at data_fs offset.
    add_custom_command(
        TARGET gen_project_binary POST_BUILD
        COMMAND "${python}" -m esptool
                --chip esp32c6
                merge_bin
                -o  "${CMAKE_BINARY_DIR}/merged.bin"
                --flash_mode dio
                --flash_size 4MB
                --flash_freq 80m
                0x0      "${CMAKE_BINARY_DIR}/bootloader/bootloader.bin"
                0x8000   "${CMAKE_BINARY_DIR}/partition_table/partition-table.bin"
                0x10000  "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
                0x210000 "${CMAKE_BINARY_DIR}/wokwi_embed_data_fs.bin"
        COMMENT "Merging flash images (+ embedded app SPIFFS) → build/merged.bin (Wokwi)"
        VERBATIM
    )

else()

    add_custom_command(
        TARGET gen_project_binary POST_BUILD
        COMMAND "${python}" -m esptool
                --chip esp32c6
                merge_bin
                -o  "${CMAKE_BINARY_DIR}/merged.bin"
                --flash_mode dio
                --flash_size 4MB
                --flash_freq 80m
                0x0     "${CMAKE_BINARY_DIR}/bootloader/bootloader.bin"
                0x8000  "${CMAKE_BINARY_DIR}/partition_table/partition-table.bin"
                0x10000 "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
        COMMENT "Merging flash images → build/merged.bin (Wokwi)"
        VERBATIM
    )

endif()
