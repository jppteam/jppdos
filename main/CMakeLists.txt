# ---- Build identity (main/jpp_ota_update.c) --------------------------------
# release.yml sets these three env vars for a tagged build: JPP_BUILD_TIMESTAMP
# (the tagged commit's committer date, as a Unix epoch — used to decide "is a
# GitHub release newer than what I'm running"), JPP_BUILD_COMMIT (short SHA,
# display-only), and JPP_BUILD_PRERELEASE (1 if this build is itself cut from
# the pre-release channel — pre-enables the "Update to pre-releases" toggle in
# Settings). A local/dev build has none of these set: it defaults to epoch 0
# (so any real release always looks newer, rather than a stale local clock
# masking a genuine update) and prerelease=1 (a dev build is not the stable
# channel, so opt it into pre-releases by default same as a develop build
# would be). See AGENTS.md "Firmware update (OTA)".
#
# Generation is skipped in the early script-mode pass that only expands
# component REQUIRES (see the MicroPython FetchContent guard in
# components/micropython/CMakeLists.txt for the same rationale) — harmless to
# skip since idf_component_register ignores SRCS/INCLUDE_DIRS in that pass.
if(DEFINED ENV{JPP_BUILD_TIMESTAMP})
    set(JPP_BUILD_TIMESTAMP "$ENV{JPP_BUILD_TIMESTAMP}")
else()
    set(JPP_BUILD_TIMESTAMP "0")
endif()
if(DEFINED ENV{JPP_BUILD_COMMIT})
    set(JPP_BUILD_COMMIT "$ENV{JPP_BUILD_COMMIT}")
else()
    set(JPP_BUILD_COMMIT "dev")
endif()
if(DEFINED ENV{JPP_BUILD_PRERELEASE})
    set(JPP_BUILD_PRERELEASE "$ENV{JPP_BUILD_PRERELEASE}")
else()
    set(JPP_BUILD_PRERELEASE "1")
endif()

set(JPP_BUILD_INFO_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${JPP_BUILD_INFO_DIR}")
if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(WRITE "${JPP_BUILD_INFO_DIR}/jpp_build_info.h"
"#pragma once
/* Generated at configure time by main/CMakeLists.txt — not checked in.
   See the \"Build identity\" comment there for what these back. */
#define JPP_BUILD_TIMESTAMP  ${JPP_BUILD_TIMESTAMP}UL
#define JPP_BUILD_COMMIT     \"${JPP_BUILD_COMMIT}\"
#define JPP_BUILD_PRERELEASE ${JPP_BUILD_PRERELEASE}
")
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "ssd1306.c"
        "jpp_boot_display.c"
        "jpp_settings_load.c"
        "jpp_file_util.c"
        "jpp_nvs_util.c"
        "jpp_bg_scheduler.c"
        "jpp_hw_init.c"
        "jpp_wifi_init.c"
        "jpp_native_services.c"
        "jpp_app_dispatch.c"
        "jpp_settings_screen.c"
        "jpp_keyboard.c"
        "jpp_onboarding.c"
        "jpp_file_picker.c"
        "jpp_icons.c"
        "jpp_serial_mgr.c"
        "jpp_backup_restore.c"
        "jpp_lrv.c"
        "jpp_lrv_server.c"
        "jpp_ota_update.c"
    INCLUDE_DIRS "." "${JPP_BUILD_INFO_DIR}"
    REQUIRES jpp_core jpp_native_loader_core jpp_crypto_core spiffs fatfs sdmmc driver esp_driver_usb_serial_jtag esp_adc json esp_wifi esp_netif lwip nvs_flash bt esp_http_client esp_timer esp_rom espressif__libsodium mbedtls esp-tls efuse spi_flash esp_partition esp_app_format
)
