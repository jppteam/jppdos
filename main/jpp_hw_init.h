#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "sdmmc_cmd.h"
#include "jpp_sd_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the I²C master bus; returns true on success. */
bool init_i2c(int sda, int scl);

/* Returns the I²C bus handle (valid after a successful init_i2c()). */
i2c_master_bus_handle_t jpp_hw_init_i2c_bus(void);

/* Mount SPIFFS partitions data_fs → /data and runtime_fs → /lib. */
bool mount_flash_storage(void);

/* Mount the SD card over SPI using the provided config. */
bool mount_sd(const jpp_sd_config_t *cfg);

/* Returns the SD card handle (valid after a successful mount_sd()). */
sdmmc_card_t *jpp_hw_init_sd_card(void);

#ifdef __cplusplus
}
#endif
