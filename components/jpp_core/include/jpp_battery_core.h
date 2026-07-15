#pragma once

#include <stdbool.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

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
#define JPP_BATTERY_OVERSAMPLE_COUNT       8        /* HARDWARE_SUMMARY.md 4.5 */

typedef struct {
    int adc_channel;        /* ADC_CHANNEL_x for the voltage-divider midpoint */
    adc_unit_t adc_unit;    /* ADC unit the channel above lives on (for calibration) */
    int full_scale_uv;      /* ADC full-scale voltage in µV (typically 3 300 000); used only when cali_handle is NULL */
    int r_top_ohm;          /* Top resistor of voltage divider (Ω) */
    int r_bot_ohm;          /* Bottom resistor of voltage divider (Ω) */
    int vbat_full_uv;       /* Battery voltage at 100 % (µV, e.g. 4 200 000 for LiPo) */
    int vbat_empty_uv;      /* Battery voltage at 0 % (µV, e.g. 3 000 000 for LiPo) */
    adc_cali_handle_t cali_handle; /* NULL until jpp_battery_cali_init() succeeds */
} jpp_battery_config_t;

typedef struct {
    int  percent;   /* 0–100 */
    bool valid;     /* false if ADC read failed or config is invalid */
    int  raw_avg;   /* averaged raw ADC count (diagnostic) */
    int  pin_uv;    /* voltage-divider midpoint voltage, µV (diagnostic) */
    int  vbat_uv;   /* reconstructed battery voltage, µV (diagnostic) */
} jpp_battery_state_t;

void jpp_battery_config_defaults(jpp_battery_config_t *cfg);

/*
 * Create an ADC calibration (curve-fitting) scheme for cfg->adc_unit /
 * cfg->adc_channel at the given attenuation and store it in cfg->cali_handle.
 * A flat full_scale_uv/raw-count formula is not accurate on ESP32-series ADCs;
 * Espressif's own calibration driver is the supported way to get an absolute
 * voltage. Call once at boot, after the channel itself has been configured.
 * On failure (e.g. missing eFuse calibration bits) cfg->cali_handle stays
 * NULL and jpp_battery_read() falls back to the linear full_scale_uv formula.
 */
void jpp_battery_cali_init(jpp_battery_config_t *cfg, adc_atten_t atten);

/*
 * Read the battery voltage via the ADC unit handle and compute a percentage.
 * The caller must have already configured the ADC channel on the unit.
 * Averages JPP_BATTERY_OVERSAMPLE_COUNT raw samples per call.
 */
void jpp_battery_read(adc_oneshot_unit_handle_t adc,
                      const jpp_battery_config_t *cfg,
                      jpp_battery_state_t *state);

#ifdef __cplusplus
}
#endif
