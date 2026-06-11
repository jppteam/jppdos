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
        "jpp_file_picker.c"
        "jpp_icons.c"
        "jpp_serial_mgr.c"
        "jpp_lrv.c"
        "jpp_lrv_server.c"
    INCLUDE_DIRS "."
    REQUIRES jpp_core jpp_native_loader_core jpp_crypto_core spiffs fatfs sdmmc driver esp_adc json esp_wifi esp_netif lwip nvs_flash bt esp_http_client esp_http_server esp_timer esp_rom espressif__libsodium mbedtls
)
