#include "../include/jpp_rtc_core.h"

#include <string.h>

#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "rtc_core";

/* ---- BCD helpers -------------------------------------------------------- */

static inline uint8_t bcd_to_bin(uint8_t bcd) { return (bcd >> 4u) * 10u + (bcd & 0x0Fu); }
static inline uint8_t bin_to_bcd(uint8_t bin) { return ((bin / 10u) << 4u) | (bin % 10u); }

/* ---- Config / init ------------------------------------------------------ */

void jpp_rtc_config_defaults(jpp_rtc_config_t *config)
{
    if (config == NULL) { return; }
    memset(config, 0, sizeof(*config));
    config->enabled = true;
    config->has_datetime = false;
    config->ntp_timeout_s = JPP_RTC_DEFAULT_NTP_TIMEOUT_S;
    config->ntp_host = JPP_RTC_DEFAULT_NTP_HOST;
    config->i2c_bus = NULL;
    config->ds1307_addr = 0x68u;
}

bool jpp_rtc_datetime_valid(const jpp_rtc_datetime_t *dt)
{
    if (dt == NULL) { return false; }
    return dt->year >= 2000 &&
           dt->month >= 1  && dt->month  <= 12 &&
           dt->day   >= 1  && dt->day    <= 31 &&
           dt->weekday >= 0 && dt->weekday <= 6 &&
           dt->hour   >= 0  && dt->hour   <= 23 &&
           dt->minute >= 0  && dt->minute <= 59 &&
           dt->second >= 0  && dt->second <= 59 &&
           dt->subseconds >= 0;
}

jpp_rtc_status_t jpp_rtc_state_init(jpp_rtc_state_t *state,
                                     const jpp_rtc_config_t *config,
                                     bool clock_available)
{
    jpp_rtc_config_t defaults;
    const jpp_rtc_config_t *src;

    if (state == NULL) { return JPP_RTC_STATUS_INVALID_ARGUMENT; }
    jpp_rtc_config_defaults(&defaults);
    src = (config == NULL) ? &defaults : config;

    memset(state, 0, sizeof(*state));
    state->enabled = src->enabled;
    state->ntp_timeout_s = src->ntp_timeout_s > 0 ? src->ntp_timeout_s : 10;
    state->ntp_host = src->ntp_host != NULL ? src->ntp_host : "time.nist.gov";

    if (src->has_datetime && jpp_rtc_datetime_valid(&src->datetime)) {
        state->has_datetime = true;
        state->datetime = src->datetime;
    }

    if (!state->enabled) {
        state->unavailable_reason = JPP_RTC_UNAVAILABLE_DISABLED;
        return JPP_RTC_STATUS_UNAVAILABLE;
    }
    if (!clock_available) {
        state->unavailable_reason = JPP_RTC_UNAVAILABLE_CLOCK_UNAVAILABLE;
        return JPP_RTC_STATUS_UNAVAILABLE;
    }

    /* Register DS1307 on the I2C bus if a handle was provided.  The DS1307 is
       optional hardware: probe the bus first, because i2c_master_bus_add_device
       only reserves the address in the driver and never touches the wire, so
       without a probe we would mark the RTC "attached" on a board that has no
       chip and then log an I2C error on every read.  Attach only when the chip
       actually answers; otherwise the firmware runs clock-less (NTP may still
       supply the time later, and all clocks fall back to "--:--"). */
    if (src->i2c_bus != NULL) {
        esp_err_t probe = i2c_master_probe(src->i2c_bus, src->ds1307_addr, 50);
        if (probe != ESP_OK) {
            ESP_LOGW(TAG, "DS1307 not detected (%s); RTC hardware disabled",
                     esp_err_to_name(probe));
            state->hw_attached = false;
        } else {
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address  = src->ds1307_addr,
                .scl_speed_hz    = 100000,
            };
            esp_err_t err = i2c_master_bus_add_device(src->i2c_bus, &dev_cfg, &state->hw_dev);
            if (err == ESP_OK) {
                state->hw_attached = true;
            } else {
                ESP_LOGW(TAG, "DS1307 attach failed: %s", esp_err_to_name(err));
                state->hw_attached = false;
            }
        }
    }

    state->ready = true;
    state->unavailable_reason = JPP_RTC_UNAVAILABLE_NONE;
    return JPP_RTC_STATUS_OK;
}

/* ---- DS1307 hardware read / write --------------------------------------- */

jpp_rtc_status_t jpp_rtc_hw_read(jpp_rtc_state_t *state)
{
    if (state == NULL) { return JPP_RTC_STATUS_INVALID_ARGUMENT; }
    if (!state->ready || !state->hw_attached) { return JPP_RTC_STATUS_UNAVAILABLE; }

    /* Read 7 DS1307 registers starting at address 0x00. */
    uint8_t reg_addr = 0x00u;
    uint8_t regs[7];
    esp_err_t err = i2c_master_transmit_receive(state->hw_dev,
                                                 &reg_addr, 1u,
                                                 regs, sizeof(regs),
                                                 50 /* timeout_ms */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS1307 read failed: %s", esp_err_to_name(err));
        /* Attempt bus reset to clear any SDA hold condition. */
        extern i2c_master_bus_handle_t jpp_hw_init_i2c_bus(void);
        i2c_master_bus_reset(jpp_hw_init_i2c_bus());
        return JPP_RTC_STATUS_HW_ERROR;
    }

    /* Clear CH (clock halt) bit in reg 0 if it is set (read-modify-write). */
    if (regs[0] & 0x80u) {
        uint8_t clear_ch[2] = { 0x00u, regs[0] & 0x7Fu };
        err = i2c_master_transmit(state->hw_dev, clear_ch, 2u, 50);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "DS1307 CH clear failed: %s", esp_err_to_name(err));
        }
        regs[0] &= 0x7Fu;
        ESP_LOGI(TAG, "RTC_CH_CLEARED");
    }

    jpp_rtc_datetime_t dt = {0};
    dt.second  = bcd_to_bin(regs[0] & 0x7Fu);
    dt.minute  = bcd_to_bin(regs[1]);
    dt.hour    = bcd_to_bin(regs[2] & 0x3Fu);  /* 24-hour mode */
    dt.weekday = regs[3];
    dt.day     = bcd_to_bin(regs[4]);
    dt.month   = bcd_to_bin(regs[5]);
    dt.year    = 2000 + bcd_to_bin(regs[6]);
    dt.subseconds = 0;

    if (!jpp_rtc_datetime_valid(&dt)) {
        ESP_LOGW(TAG, "DS1307 returned invalid time %04d-%02d-%02d %02d:%02d:%02d",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
        return JPP_RTC_STATUS_INVALID_TIME;
    }

    state->datetime = dt;
    state->has_datetime = true;
    state->last_hw_read_us = esp_timer_get_time();
    ESP_LOGI(TAG, "RTC_READ %04d-%02d-%02d %02d:%02d:%02d",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    return JPP_RTC_STATUS_OK;
}

jpp_rtc_status_t jpp_rtc_hw_write(jpp_rtc_state_t *state,
                                   const jpp_rtc_datetime_t *dt)
{
    if (state == NULL || dt == NULL) { return JPP_RTC_STATUS_INVALID_ARGUMENT; }
    if (!state->ready || !state->hw_attached) { return JPP_RTC_STATUS_UNAVAILABLE; }
    if (!jpp_rtc_datetime_valid(dt)) { return JPP_RTC_STATUS_INVALID_TIME; }

    /* Write registers 0x00-0x06: sec (CH cleared), min, hour, wday, day, mon, year */
    uint8_t buf[8] = {
        0x00u,                           /* register address */
        bin_to_bcd((uint8_t)dt->second) & 0x7Fu,  /* CH=0 */
        bin_to_bcd((uint8_t)dt->minute),
        bin_to_bcd((uint8_t)dt->hour) & 0x3Fu,  /* 24h mode */
        (uint8_t)dt->weekday,
        bin_to_bcd((uint8_t)dt->day),
        bin_to_bcd((uint8_t)dt->month),
        bin_to_bcd((uint8_t)(dt->year - 2000)),
    };
    esp_err_t err = i2c_master_transmit(state->hw_dev, buf, sizeof(buf), 50);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS1307 write failed: %s", esp_err_to_name(err));
        return JPP_RTC_STATUS_HW_ERROR;
    }

    state->datetime = *dt;
    state->has_datetime = true;
    state->last_hw_read_us = esp_timer_get_time();
    return JPP_RTC_STATUS_OK;
}

/* ---- Live time from software tick --------------------------------------- */

jpp_rtc_status_t jpp_rtc_get_current(const jpp_rtc_state_t *state,
                                      jpp_rtc_datetime_t *out)
{
    if (state == NULL || out == NULL) { return JPP_RTC_STATUS_INVALID_ARGUMENT; }
    if (!state->has_datetime) { return JPP_RTC_STATUS_UNAVAILABLE; }

    *out = state->datetime;

    if (state->last_hw_read_us == 0) { return JPP_RTC_STATUS_OK; }

    int64_t elapsed_us = esp_timer_get_time() - state->last_hw_read_us;
    if (elapsed_us <= 0) { return JPP_RTC_STATUS_OK; }

    /* Add elapsed seconds (integer part only). */
    int elapsed_s = (int)(elapsed_us / 1000000LL);
    if (elapsed_s <= 0) { return JPP_RTC_STATUS_OK; }

    static const int days_in_month[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

    out->second += elapsed_s;
    while (out->second >= 60) { out->second -= 60; out->minute++; }
    while (out->minute >= 60) { out->minute -= 60; out->hour++;   }
    while (out->hour   >= 24) {
        out->hour -= 24;
        out->day++;
        int month_idx = (out->month - 1) % 12;
        int max_day = days_in_month[month_idx];
        /* Leap year for February */
        if (month_idx == 1 && (out->year % 4 == 0) &&
            (out->year % 100 != 0 || out->year % 400 == 0)) {
            max_day = 29;
        }
        if (out->day > max_day) {
            out->day = 1;
            out->month++;
            if (out->month > 12) { out->month = 1; out->year++; }
        }
    }
    return JPP_RTC_STATUS_OK;
}

/* ---- State setters ------------------------------------------------------ */

jpp_rtc_status_t jpp_rtc_set_time(jpp_rtc_state_t *state,
                                   const jpp_rtc_datetime_t *datetime_value)
{
    if (state == NULL) { return JPP_RTC_STATUS_INVALID_ARGUMENT; }
    if (!state->ready) { return JPP_RTC_STATUS_UNAVAILABLE; }
    if (datetime_value == NULL) {
        state->has_datetime = false;
        memset(&state->datetime, 0, sizeof(state->datetime));
        return JPP_RTC_STATUS_OK;
    }
    if (!jpp_rtc_datetime_valid(datetime_value)) { return JPP_RTC_STATUS_INVALID_TIME; }
    state->datetime = *datetime_value;
    state->has_datetime = true;
    state->last_hw_read_us = esp_timer_get_time();
    return JPP_RTC_STATUS_OK;
}

jpp_rtc_status_t jpp_rtc_sync_ntp(jpp_rtc_state_t *state,
                                   const char *ntp_host,
                                   const jpp_rtc_datetime_t *resolved_datetime,
                                   bool ntp_available)
{
    if (state == NULL) { return JPP_RTC_STATUS_INVALID_ARGUMENT; }
    if (!state->ready) { return JPP_RTC_STATUS_UNAVAILABLE; }
    state->ntp_host = (ntp_host != NULL && ntp_host[0] != '\0') ? ntp_host : "time.nist.gov";
    if (!ntp_available) { return JPP_RTC_STATUS_NTP_TIMEOUT; }
    return jpp_rtc_set_time(state, resolved_datetime);
}

/* ---- Name helpers ------------------------------------------------------- */

const char *jpp_rtc_status_name(jpp_rtc_status_t status)
{
    switch (status) {
    case JPP_RTC_STATUS_OK:               return "OK";
    case JPP_RTC_STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case JPP_RTC_STATUS_INVALID_TIME:     return "INVALID_TIME";
    case JPP_RTC_STATUS_UNAVAILABLE:      return "UNAVAILABLE";
    case JPP_RTC_STATUS_NTP_TIMEOUT:      return "NTP_TIMEOUT";
    case JPP_RTC_STATUS_HW_ERROR:         return "HW_ERROR";
    }
    return "UNKNOWN";
}

const char *jpp_rtc_unavailable_reason_name(jpp_rtc_unavailable_reason_t reason)
{
    switch (reason) {
    case JPP_RTC_UNAVAILABLE_NONE:              return "none";
    case JPP_RTC_UNAVAILABLE_DISABLED:          return "disabled";
    case JPP_RTC_UNAVAILABLE_CLOCK_UNAVAILABLE: return "clock_unavailable";
    }
    return "unknown";
}
