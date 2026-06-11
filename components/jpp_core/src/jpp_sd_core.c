#include "../include/jpp_sd_core.h"

#include <string.h>

void jpp_sd_config_defaults(jpp_sd_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->has_sck = true;
    config->has_mosi = true;
    config->has_miso = true;
    config->has_cs = true;
    config->sck = 14;
    config->mosi = 15;
    config->miso = 18;
    config->cs = 7;
}
