#pragma once

#include <stdbool.h>

#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default values used by jpp_battery_config_defaults(). */
#define JPP_BATTERY_DEFAULT_ADC_CHANNEL    4        /* ADC_CHANNEL_4 = GPIO4 */
#define JPP_BATTERY_DEFAULT_FULL_SCALE_UV  3300000
#define JPP_BATTERY_DEFAULT_R_TOP_OHM      220000
#define JPP_BATTERY_DEFAULT_R_BOT_OHM      220000
#define JPP_BATTERY_DEFAULT_VBAT_FULL_UV   4200000
#define JPP_BATTERY_DEFAULT_VBAT_EMPTY_UV  3000000
#define JPP_BATTERY_ADC_MAX_RAW            4095LL   /* 12-bit ADC full-scale */

typedef struct {
    int adc_channel;        /* ADC_CHANNEL_x for the voltage-divider midpoint */
    int full_scale_uv;      /* ADC full-scale voltage in µV (typically 3 300 000) */
    int r_top_ohm;          /* Top resistor of voltage divider (Ω) */
    int r_bot_ohm;          /* Bottom resistor of voltage divider (Ω) */
    int vbat_full_uv;       /* Battery voltage at 100 % (µV, e.g. 4 200 000 for LiPo) */
    int vbat_empty_uv;      /* Battery voltage at 0 % (µV, e.g. 3 000 000 for LiPo) */
} jpp_battery_config_t;

typedef struct {
    int  percent;   /* 0–100 */
    bool valid;     /* false if ADC read failed or config is invalid */
} jpp_battery_state_t;

void jpp_battery_config_defaults(jpp_battery_config_t *cfg);

/*
 * Read the battery voltage via the ADC unit handle and compute a percentage.
 * The caller must have already configured the ADC channel on the unit.
 */
void jpp_battery_read(adc_oneshot_unit_handle_t adc,
                      const jpp_battery_config_t *cfg,
                      jpp_battery_state_t *state);

#ifdef __cplusplus
}
#endif
