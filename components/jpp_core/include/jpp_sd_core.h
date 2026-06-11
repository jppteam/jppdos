#pragma once

/*
 * SD card SPI pin configuration.  The card itself is mounted by the firmware
 * layer (main/jpp_hw_init.c) via esp_vfs_fat_sdspi_mount; this header only
 * carries the pin assignment between the board config and the mount call.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool has_sck;
    bool has_mosi;
    bool has_miso;
    bool has_cs;
    int sck;
    int mosi;
    int miso;
    int cs;
} jpp_sd_config_t;

void jpp_sd_config_defaults(jpp_sd_config_t *config);

#ifdef __cplusplus
}
#endif
