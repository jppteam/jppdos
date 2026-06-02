#include "../include/jpp_battery_core.h"

void jpp_battery_config_defaults(jpp_battery_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->adc_channel    = JPP_BATTERY_DEFAULT_ADC_CHANNEL;
    cfg->full_scale_uv  = JPP_BATTERY_DEFAULT_FULL_SCALE_UV;
    cfg->r_top_ohm      = JPP_BATTERY_DEFAULT_R_TOP_OHM;
    cfg->r_bot_ohm      = JPP_BATTERY_DEFAULT_R_BOT_OHM;
    cfg->vbat_full_uv   = JPP_BATTERY_DEFAULT_VBAT_FULL_UV;
    cfg->vbat_empty_uv  = JPP_BATTERY_DEFAULT_VBAT_EMPTY_UV;
}

void jpp_battery_read(adc_oneshot_unit_handle_t adc,
                      const jpp_battery_config_t *cfg,
                      jpp_battery_state_t *state)
{
    int raw = 0;
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

    if (adc_oneshot_read(adc, (adc_channel_t)cfg->adc_channel, &raw) != ESP_OK) {
        state->valid   = false;
        state->percent = 0;
        return;
    }

    /* Convert raw ADC reading to pin voltage in µV. */
    pin_uv = (int)((int64_t)raw * cfg->full_scale_uv / JPP_BATTERY_ADC_MAX_RAW);

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

    state->percent = percent;
    state->valid   = true;
}
