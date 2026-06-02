#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default values used by jpp_oled_config_defaults(). */
#define JPP_OLED_DEFAULT_WIDTH    128
#define JPP_OLED_DEFAULT_HEIGHT   64
#define JPP_OLED_DEFAULT_I2C_ID   0
#define JPP_OLED_DEFAULT_SDA_PIN  20
#define JPP_OLED_DEFAULT_SCL_PIN  19
#define JPP_OLED_DEFAULT_FREQ_HZ  100000
#define JPP_OLED_DEFAULT_ADDRESS  0x3Cu

typedef enum {
    JPP_OLED_STATUS_OK = 0,
    JPP_OLED_STATUS_INVALID_ARGUMENT,
    JPP_OLED_STATUS_UNAVAILABLE,
} jpp_oled_status_t;

typedef enum {
    JPP_OLED_UNAVAILABLE_NONE = 0,
    JPP_OLED_UNAVAILABLE_DISABLED,
    JPP_OLED_UNAVAILABLE_CONFIG_MISSING,
    JPP_OLED_UNAVAILABLE_I2C_UNAVAILABLE,
} jpp_oled_unavailable_reason_t;

typedef struct {
    bool enabled;
    bool has_width;
    bool has_height;
    bool has_i2c_id;
    bool has_sda_pin;
    bool has_scl_pin;
    bool has_freq;
    bool has_address;
    int width;
    int height;
    int i2c_id;
    int sda_pin;
    int scl_pin;
    int freq;
    uint8_t address;
} jpp_oled_config_t;

typedef struct {
    bool available;
    bool enabled;
    jpp_oled_unavailable_reason_t unavailable_reason;
    int width;
    int height;
    int i2c_id;
    int sda_pin;
    int scl_pin;
    int freq;
    uint8_t address;
    size_t frame_bytes;
} jpp_oled_state_t;

void jpp_oled_config_defaults(jpp_oled_config_t *config);
jpp_oled_status_t jpp_oled_state_init(
    jpp_oled_state_t *state,
    const jpp_oled_config_t *config,
    bool i2c_available
);
jpp_oled_status_t jpp_oled_frame_bytes(const jpp_oled_state_t *state, size_t *frame_bytes);
const char *jpp_oled_status_name(jpp_oled_status_t status);
const char *jpp_oled_unavailable_reason_name(jpp_oled_unavailable_reason_t reason);

#ifdef __cplusplus
}
#endif
