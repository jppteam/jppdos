# ---------------------------------------------------------------------------
# Games — native hub app + dynamically loaded game modules for the J++Device.
#
# The ESP-IDF component registration below is kept for IDE support (include
# paths, code navigation, static analysis) but this component is NOT linked
# into the firmware. The firmware loads the hub at runtime from the SD card
# using jpp_native_loader_core; the hub then loads one <name>.mod.bin game
# module at a time into the app-pool tail via jpp_sdk_module_load().
#
# To build the loadable binaries, run (from the project root, inside the
# Docker build environment):
#
#   idf.py build            # also triggers the games_bin custom target
#
# The output is build/apps/games/games.bin (ELF32 shared object), one
# <name>.mod.bin per game, and build/apps/games/manifest.json — copy the
# directory contents to /sd/apps/games/ on the SD card.
# ---------------------------------------------------------------------------

idf_component_register(
    SRCS
        "src/hub.c"
        "src/games_entry.c"
        "src/games_gfx.c"
        "src/games_store.c"
        "src/games_sfx.c"
        "src/games_rng.c"
        "src/games_ble.c"
        "src/mod/tetris.c"
        "src/mod/pong.c"
        "src/mod/snake.c"
        "src/mod/breakout.c"
        "src/mod/g2048.c"
        "src/mod/flappy.c"
        "src/mod/racer.c"
        "src/mod/connect4.c"
        "src/mod/battleship.c"
    INCLUDE_DIRS "include"
    REQUIRES jpp_core freertos
)

# ---------------------------------------------------------------------------
# Standalone shared-library targets (games.bin + *.mod.bin).
# ---------------------------------------------------------------------------

set(_GAMES_BIN ${CMAKE_BINARY_DIR}/apps/games/games.bin)

idf_build_get_property(_games_python PYTHON)

add_custom_target(games_bin ALL
    COMMAND ${_games_python}
            ${CMAKE_CURRENT_SOURCE_DIR}/build_shared.py
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building Games shared library + modules → ${_GAMES_BIN}"
    DEPENDS gen_project_binary
    VERBATIM
)
