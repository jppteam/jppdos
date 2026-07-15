#pragma once

/*
 * AT24C32-class I2C EEPROM driver.
 *
 * Many DS1307 RTC breakout boards ("Tiny RTC" modules) carry a companion
 * AT24C32 serial EEPROM at I2C address 0x50, sharing the RTC's I2C bus.  The
 * chip is 4 KB, organised in 32-byte write pages with a ~5 ms write cycle and a
 * 2-byte (big-endian) word address.
 *
 * Like the DS1307 the EEPROM is OPTIONAL: jpp_eeprom_state_init() probes the bus
 * and only marks the chip present when it actually ACKs, so a board without the
 * EEPROM simply reports "not present" and every consumer falls back gracefully.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AT24C32: 32 bytes per write page, ~5 ms self-timed write cycle (10 ms max).
 * The driver ACK-polls the chip after every page write rather than using a
 * fixed delay (a fixed vTaskDelay rounds to zero ticks at low CONFIG_FREERTOS_HZ
 * and would let a still-busy chip NACK the next page). WRITE_CYCLE_MAX_MS caps
 * how long the ACK-poll waits before giving up. */
#define JPP_EEPROM_PAGE_SIZE          32u
#define JPP_EEPROM_WRITE_CYCLE_MAX_MS 25u

typedef enum {
    JPP_EEPROM_OK = 0,
    JPP_EEPROM_ERR_INVALID_ARGUMENT,
    JPP_EEPROM_ERR_NOT_PRESENT,
    JPP_EEPROM_ERR_OUT_OF_RANGE,
    JPP_EEPROM_ERR_IO,
} jpp_eeprom_result_t;

typedef struct {
    bool                    present;     /* chip ACKed at init                 */
    size_t                  size_bytes;  /* total capacity                     */
    i2c_master_dev_handle_t hw_dev;      /* created when present               */
    i2c_master_bus_handle_t bus;         /* bus handle (for write-cycle ACK poll) */
    uint8_t                 i2c_addr;    /* device address (for ACK poll)      */
} jpp_eeprom_state_t;

/*
 * Probe the EEPROM at i2c_addr on the given bus.  On ACK the device is attached
 * and state->present is set true; otherwise state->present is false and reads /
 * writes return JPP_EEPROM_ERR_NOT_PRESENT.  bus may be NULL (no hardware).
 */
jpp_eeprom_result_t jpp_eeprom_state_init(jpp_eeprom_state_t *state,
                                          i2c_master_bus_handle_t bus,
                                          uint8_t i2c_addr,
                                          size_t size_bytes);

/* True if the chip answered at init. */
bool jpp_eeprom_present(const jpp_eeprom_state_t *state);

/* Sequential read of len bytes starting at addr into buf. */
jpp_eeprom_result_t jpp_eeprom_read(const jpp_eeprom_state_t *state,
                                    uint16_t addr, uint8_t *buf, size_t len);

/*
 * Write len bytes from buf starting at addr.  Handles page-boundary splitting
 * and waits out each page's write cycle, so len may span multiple pages.
 */
jpp_eeprom_result_t jpp_eeprom_write(jpp_eeprom_state_t *state,
                                     uint16_t addr, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
