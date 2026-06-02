#include "jpp_hw_init.h"

#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"

static const char *TAG = "hw_init";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static sdmmc_card_t           *s_sd_card = NULL;

bool init_i2c(int sda, int scl)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port                     = I2C_NUM_0,
        .sda_io_num                   = (gpio_num_t)sda,
        .scl_io_num                   = (gpio_num_t)scl,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C init: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

i2c_master_bus_handle_t jpp_hw_init_i2c_bus(void)
{
    return s_i2c_bus;
}

bool mount_flash_storage(void)
{
    esp_vfs_spiffs_conf_t data_conf = {
        .base_path              = "/data",
        .partition_label        = "data_fs",
        .max_files              = 8,
        .format_if_mount_failed = true,
    };
    if (esp_vfs_spiffs_register(&data_conf) != ESP_OK) {
        ESP_LOGE(TAG, "data_fs mount failed");
        return false;
    }
    esp_vfs_spiffs_conf_t lib_conf = {
        .base_path              = "/lib",
        .partition_label        = "runtime_fs",
        .max_files              = 8,
        .format_if_mount_failed = true,
    };
    if (esp_vfs_spiffs_register(&lib_conf) != ESP_OK) {
        ESP_LOGE(TAG, "runtime_fs mount failed");
        return false;
    }
    mkdir("/data/apps", 0755);
    return true;
}

sdmmc_card_t *jpp_hw_init_sd_card(void)
{
    return s_sd_card;
}

bool mount_sd(const jpp_sd_config_t *cfg)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = cfg->mosi,
        .miso_io_num     = cfg->miso,
        .sclk_io_num     = cfg->sck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI bus init: %s", esp_err_to_name(err));
        return false;
    }
    sdmmc_host_t          host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t dev  = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = (gpio_num_t)cfg->cs;
    dev.host_id = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount("/sd", &host, &dev, &mcfg, &s_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}
