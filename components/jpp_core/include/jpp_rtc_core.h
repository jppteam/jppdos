#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_RTC_DEFAULT_NTP_TIMEOUT_S  10
#define JPP_RTC_DEFAULT_NTP_HOST       "time.nist.gov"

typedef enum {
    JPP_RTC_STATUS_OK = 0,
    JPP_RTC_STATUS_INVALID_ARGUMENT,
    JPP_RTC_STATUS_INVALID_TIME,
    JPP_RTC_STATUS_UNAVAILABLE,
    JPP_RTC_STATUS_NTP_TIMEOUT,
    JPP_RTC_STATUS_HW_ERROR,
} jpp_rtc_status_t;

typedef enum {
    JPP_RTC_UNAVAILABLE_NONE = 0,
    JPP_RTC_UNAVAILABLE_DISABLED,
    JPP_RTC_UNAVAILABLE_CLOCK_UNAVAILABLE,
} jpp_rtc_unavailable_reason_t;

typedef struct {
    int year;
    int month;
    int day;
    int weekday;
    int hour;
    int minute;
    int second;
    int subseconds;
} jpp_rtc_datetime_t;

typedef struct {
    bool enabled;
    bool has_datetime;
    jpp_rtc_datetime_t datetime;
    int ntp_timeout_s;
    const char *ntp_host;
    /* Optional: I2C bus handle for DS1307 hardware reads.  NULL = no hw. */
    i2c_master_bus_handle_t i2c_bus;
    uint8_t ds1307_addr;
} jpp_rtc_config_t;

typedef struct {
    bool ready;
    bool enabled;
    bool has_datetime;
    jpp_rtc_datetime_t datetime;
    int ntp_timeout_s;
    const char *ntp_host;
    jpp_rtc_unavailable_reason_t unavailable_reason;
    /* Hardware I2C device handle (created by jpp_rtc_state_init when bus provided) */
    i2c_master_dev_handle_t hw_dev;
    bool hw_attached;
    /* Software tick: microseconds since last hw read (from esp_timer_get_time) */
    int64_t last_hw_read_us;
} jpp_rtc_state_t;

void jpp_rtc_config_defaults(jpp_rtc_config_t *config);
bool jpp_rtc_datetime_valid(const jpp_rtc_datetime_t *datetime_value);
jpp_rtc_status_t jpp_rtc_state_init(jpp_rtc_state_t *state,
                                     const jpp_rtc_config_t *config,
                                     bool clock_available);

/* Read the DS1307 over I2C and update state->datetime + last_hw_read_us.
   Clears the CH (clock halt) bit if it is set.  Requires hw_attached. */
jpp_rtc_status_t jpp_rtc_hw_read(jpp_rtc_state_t *state);

/* Write a new datetime to the DS1307 hardware registers. */
jpp_rtc_status_t jpp_rtc_hw_write(jpp_rtc_state_t *state,
                                   const jpp_rtc_datetime_t *dt);

/* Compute the current datetime by adding elapsed time since last hw read.
   Falls back to the stored datetime if hw has never been read. */
jpp_rtc_status_t jpp_rtc_get_current(const jpp_rtc_state_t *state,
                                      jpp_rtc_datetime_t *out);

jpp_rtc_status_t jpp_rtc_set_time(jpp_rtc_state_t *state,
                                   const jpp_rtc_datetime_t *datetime_value);
jpp_rtc_status_t jpp_rtc_sync_ntp(jpp_rtc_state_t *state,
                                   const char *ntp_host,
                                   const jpp_rtc_datetime_t *resolved_datetime,
                                   bool ntp_available);

const char *jpp_rtc_status_name(jpp_rtc_status_t status);
const char *jpp_rtc_unavailable_reason_name(jpp_rtc_unavailable_reason_t reason);

#ifdef __cplusplus
}
#endif
