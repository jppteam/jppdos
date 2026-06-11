#pragma once

/*
 * Hardware pin and peripheral configuration for J++Device (ESP32-C6 Super Mini).
 *
 * All GPIO numbers, ADC channels, and hardware addresses are defined here.
 * Edit this file to adapt the firmware to a different board layout.
 */

#include "esp_adc/adc_oneshot.h"

/* ---- I2C / OLED --------------------------------------------------------- */
/*
 * Shared I2C bus (SSD1306 OLED @0x3C + DS1307 RTC @0x68 on real hardware).
 * Speed is capped at 100 kHz: the DS1307 RTC tops out there and both devices
 * share the bus, so the OLED must run at the same rate.  See HARDWARE_SUMMARY.md
 * §3 / §5.6.
 */
#define JPP_HW_OLED_SDA_PIN      20
#define JPP_HW_OLED_SCL_PIN      19
#define JPP_HW_OLED_I2C_ADDR     0x3Cu
#define JPP_HW_OLED_I2C_PORT     I2C_NUM_0
#define JPP_HW_OLED_I2C_FREQ_HZ  100000

/* ---- SD card (SPI) ------------------------------------------------------ */
/*
 * Shared SPI2 bus (SD card + SX1276 LoRa).  CLK=14, MOSI=15, MISO=18.
 * SD chip-select = GPIO7. GPIO6 is wired to the LoRa radio's CS and is
 * reserved on the bus; the firmware does not drive it.
 */
#define JPP_HW_SD_SCK_PIN        14
#define JPP_HW_SD_MOSI_PIN       15
#define JPP_HW_SD_MISO_PIN       18
#define JPP_HW_SD_CS_PIN         7

/* ---- Keypad (resistor-ladder ADC) --------------------------------------- */
/*
 * 5-key resistor ladder on ADC1 channel 2 (GPIO2).  The ESP32-C6 internal
 * ~45 kΩ pull-up is the TOP of the divider (HARDWARE_SUMMARY.md §4.6 / §5.4):
 * it must be enabled on the ADC pin or it floats and ghost-presses.  With no
 * key down the pin sits high (~2.7 V = NONE); each key shorts part of the
 * ladder, pulling the voltage down (LEFT = full short ≈ 0 V).
 */
#define JPP_HW_KEYPAD_ADC_UNIT   ADC_UNIT_1
#define JPP_HW_KEYPAD_ADC_CH     ADC_CHANNEL_2   /* GPIO 2 */
#define JPP_HW_KEYPAD_GPIO       2               /* same pin, for pull-up cfg */
#define JPP_HW_KEYPAD_FULL_SCALE_UV 3300000

/* ---- Battery (voltage divider ADC) -------------------------------------- */
/*
 * Battery is connected through two 220 kΩ resistors in series (voltage
 * divider).  The midpoint feeds this ADC channel.  Vpin = Vbat / 2.
 * ADC1 channel 1 = GPIO1.
 */
#define JPP_HW_BATTERY_ADC_CH    ADC_CHANNEL_1   /* GPIO 1 */
#define JPP_HW_BATTERY_GPIO      1
#define JPP_HW_BATTERY_R_TOP_OHM 220000
#define JPP_HW_BATTERY_R_BOT_OHM 220000

/* ---- ADC resolution ----------------------------------------------------- */
/* All ADC channels on ESP32-C6 are configured at 12-bit resolution.         */
#define JPP_HW_ADC_12BIT_MAX_RAW  4095LL

/* ---- Buzzer (passive piezo, LEDC PWM) ----------------------------------- */
/*
 * GPIO3 is an LP (low-power) pad with ~5 mA default drive.  Firmware must call
 * gpio_set_drive_capability(GPIO_NUM_3, GPIO_DRIVE_CAP_3) for adequate volume.
 */
#define JPP_HW_BUZZER_GPIO           3
#define JPP_HW_BUZZER_LEDC_TIMER     LEDC_TIMER_0
#define JPP_HW_BUZZER_LEDC_CHANNEL   LEDC_CHANNEL_0
#define JPP_HW_BUZZER_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define JPP_HW_BUZZER_RESOLUTION     LEDC_TIMER_10_BIT
#define JPP_HW_BUZZER_DUTY           512u   /* 50 % duty at 10-bit resolution */

/* ---- DS1307 RTC (I2C 0x68) ---------------------------------------------- */
#define JPP_HW_DS1307_I2C_ADDR       0x68u
#define JPP_HW_DS1307_I2C_TIMEOUT_MS 50u

/* ---- Wokwi GPIO simulation shim ---------------------------------------- */
/*
 * SIMULATION ONLY.  These pins exist solely for the Wokwi keypad shim and are
 * compiled in only when JPP_WOKWI_SIM is defined (set by the Wokwi build, see
 * the top-level CMakeLists.txt).  On a real-hardware build the shim is absent,
 * so these GPIOs are never touched by the firmware.
 *
 * They are deliberately kept OFF every physical-bus / ADC pin in the
 * documented hardware map (I2C 19/20, SPI 14/15/18, SD CS 7, keypad ADC 2,
 * battery ADC 1) so the simulated push-buttons cannot collide with a real
 * peripheral.  Wokwi's diagram.json wires the buttons to these same GPIOs.
 */
#define JPP_HW_SIM_UP_GPIO       0
#define JPP_HW_SIM_DOWN_GPIO     10
#define JPP_HW_SIM_LEFT_GPIO     11
#define JPP_HW_SIM_RIGHT_GPIO    21
#define JPP_HW_SIM_CENTER_GPIO   22
