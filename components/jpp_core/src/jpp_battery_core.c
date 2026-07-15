#include "../include/jpp_battery_core.h"

#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "jpp_battery";

void jpp_battery_config_defaults(jpp_battery_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->adc_channel    = JPP_BATTERY_DEFAULT_ADC_CHANNEL;
    cfg->adc_unit       = ADC_UNIT_1;
    cfg->full_scale_uv  = JPP_BATTERY_DEFAULT_FULL_SCALE_UV;
    cfg->r_top_ohm      = JPP_BATTERY_DEFAULT_R_TOP_OHM;
    cfg->r_bot_ohm      = JPP_BATTERY_DEFAULT_R_BOT_OHM;
    cfg->vbat_full_uv   = JPP_BATTERY_DEFAULT_VBAT_FULL_UV;
    cfg->vbat_empty_uv  = JPP_BATTERY_DEFAULT_VBAT_EMPTY_UV;
    cfg->cali_handle    = NULL;
}

void jpp_battery_cali_init(jpp_battery_config_t *cfg, adc_atten_t atten)
{
    if (cfg == NULL) {
        return;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = cfg->adc_unit,
        .atten    = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_handle_t handle = NULL;
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &handle);
    if (err == ESP_OK) {
        cfg->cali_handle = handle;
    } else {
        ESP_LOGW(TAG, "ADC calibration scheme unavailable (err=%d); "
                       "falling back to linear full_scale_uv math", err);
        cfg->cali_handle = NULL;
    }
}

void jpp_battery_read(adc_oneshot_unit_handle_t adc,
                      const jpp_battery_config_t *cfg,
                      jpp_battery_state_t *state)
{
    int64_t raw_sum = 0;
    int raw_avg;
    int pin_uv;
    int vbat_uv;
    int range_uv;
    int percent;

    if (adc == NULL || cfg == NULL || state == NULL) {
        if (state != NULL) {
            state->valid   = false;
            state->percent = 0;
        }
        return;
    }

    for (int i = 0; i < JPP_BATTERY_OVERSAMPLE_COUNT; i++) {
        int sample = 0;
        if (adc_oneshot_read(adc, (adc_channel_t)cfg->adc_channel, &sample) != ESP_OK) {
            state->valid   = false;
            state->percent = 0;
            return;
        }
        raw_sum += sample;
    }
    raw_avg = (int)(raw_sum / JPP_BATTERY_OVERSAMPLE_COUNT);

    /* Prefer the calibrated curve (accounts for chip-to-chip ADC nonlinearity);
       fall back to the flat full_scale_uv formula if calibration isn't available. */
    bool have_cali_pin_uv = false;
    if (cfg->cali_handle != NULL) {
        int pin_mv = 0;
        if (adc_cali_raw_to_voltage(cfg->cali_handle, raw_avg, &pin_mv) == ESP_OK) {
            pin_uv = pin_mv * 1000;
            have_cali_pin_uv = true;
        }
    }
    if (!have_cali_pin_uv) {
        pin_uv = (int)((int64_t)raw_avg * cfg->full_scale_uv / JPP_BATTERY_ADC_MAX_RAW);
    }

    /* Reconstruct battery voltage from voltage divider: Vbat = Vpin * (R_top + R_bot) / R_bot */
    vbat_uv = (int)((int64_t)pin_uv * (cfg->r_top_ohm + cfg->r_bot_ohm) / cfg->r_bot_ohm);

    range_uv = cfg->vbat_full_uv - cfg->vbat_empty_uv;
    if (range_uv <= 0) {
        state->valid   = false;
        state->percent = 0;
        return;
    }

    percent = (int)((int64_t)(vbat_uv - cfg->vbat_empty_uv) * 100LL / range_uv);
    if (percent < 0)   { percent = 0; }
    if (percent > 100) { percent = 100; }

    state->raw_avg = raw_avg;
    state->pin_uv  = pin_uv;
    state->vbat_uv = vbat_uv;
    state->percent = percent;
    state->valid   = true;
}
