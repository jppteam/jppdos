#include "../include/jpp_eeprom_core.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "eeprom_core";

/* I2C transaction timeout (ms). */
#define EEPROM_I2C_TIMEOUT_MS 100

jpp_eeprom_result_t jpp_eeprom_state_init(jpp_eeprom_state_t *state,
                                          i2c_master_bus_handle_t bus,
                                          uint8_t i2c_addr,
                                          size_t size_bytes)
{
    if (state == NULL) { return JPP_EEPROM_ERR_INVALID_ARGUMENT; }
    memset(state, 0, sizeof(*state));
    state->size_bytes = size_bytes;

    if (bus == NULL) {
        state->present = false;
        return JPP_EEPROM_ERR_NOT_PRESENT;
    }

    /* Probe first: like the DS1307, the EEPROM is optional.  i2c_master_probe
       touches the wire and only ACKs when the chip is actually fitted. */
    esp_err_t probe = i2c_master_probe(bus, i2c_addr, 50);
    if (probe != ESP_OK) {
        ESP_LOGW(TAG, "AT24C32 not detected at 0x%02X (%s)", i2c_addr,
                 esp_err_to_name(probe));
        state->present = false;
        return JPP_EEPROM_ERR_NOT_PRESENT;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = i2c_addr,
        .scl_speed_hz    = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &state->hw_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AT24C32 attach failed: %s", esp_err_to_name(err));
        state->present = false;
        return JPP_EEPROM_ERR_IO;
    }

    state->bus      = bus;
    state->i2c_addr = i2c_addr;
    state->present  = true;
    ESP_LOGI(TAG, "AT24C32 attached at 0x%02X (%u bytes)", i2c_addr,
             (unsigned)size_bytes);
    return JPP_EEPROM_OK;
}

bool jpp_eeprom_present(const jpp_eeprom_state_t *state)
{
    return state != NULL && state->present;
}

jpp_eeprom_result_t jpp_eeprom_read(const jpp_eeprom_state_t *state,
                                    uint16_t addr, uint8_t *buf, size_t len)
{
    if (state == NULL || buf == NULL) { return JPP_EEPROM_ERR_INVALID_ARGUMENT; }
    if (!state->present)               { return JPP_EEPROM_ERR_NOT_PRESENT; }
    if (len == 0u)                     { return JPP_EEPROM_OK; }
    if ((size_t)addr + len > state->size_bytes) { return JPP_EEPROM_ERR_OUT_OF_RANGE; }

    /* Set the word address (2 bytes, big-endian) then sequentially read. */
    uint8_t waddr[2] = { (uint8_t)(addr >> 8u), (uint8_t)(addr & 0xFFu) };
    esp_err_t err = i2c_master_transmit_receive(state->hw_dev,
                                                waddr, sizeof(waddr),
                                                buf, len,
                                                EEPROM_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read @0x%04X len %u: %s", addr, (unsigned)len,
                 esp_err_to_name(err));
        return JPP_EEPROM_ERR_IO;
    }
    return JPP_EEPROM_OK;
}

/* Block until the chip finishes its self-timed write cycle, using ACK polling:
   the AT24 does not ACK its device address while an internal write is in
   progress, so we probe until it ACKs (ready) or the max cycle time elapses.
   This replaces a fixed vTaskDelay, which rounds to zero ticks at low
   CONFIG_FREERTOS_HZ (100 Hz here) and would let a still-busy chip NACK the
   next page write.

   The per-probe timeout MUST round to at least one FreeRTOS tick. At
   CONFIG_FREERTOS_HZ = 100, pdMS_TO_TICKS() of anything under 10 ms yields
   ZERO ticks, and i2c_master_probe() then returns ESP_ERR_TIMEOUT *before*
   the I2C peripheral finishes the START/STOP and reports the ACK/NACK -- so
   every probe "fails" and the chip is never seen to become ready, timing out
   after WRITE_CYCLE_MAX_MS even though the write physically completed. This is
   the very tick-rounding trap the fixed vTaskDelay above fell into, so keep the
   probe timeout >= JPP_EEPROM_WRITE_CYCLE_MAX_MS (a NACK still returns as soon
   as the hardware aborts the transaction, so a busy chip does not stall here). */
static jpp_eeprom_result_t wait_write_complete(jpp_eeprom_state_t *state)
{
    int64_t deadline = esp_timer_get_time()
                     + (int64_t)JPP_EEPROM_WRITE_CYCLE_MAX_MS * 1000;
    for (;;) {
        if (i2c_master_probe(state->bus, state->i2c_addr,
                             JPP_EEPROM_WRITE_CYCLE_MAX_MS) == ESP_OK) {
            return JPP_EEPROM_OK;
        }
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGW(TAG, "write cycle did not complete within %u ms",
                     (unsigned)JPP_EEPROM_WRITE_CYCLE_MAX_MS);
            return JPP_EEPROM_ERR_IO;
        }
    }
}

/* Write one page-bounded chunk (caller guarantees it does not cross a page). */
static jpp_eeprom_result_t write_page(jpp_eeprom_state_t *state,
                                      uint16_t addr, const uint8_t *buf, size_t len)
{
    uint8_t frame[2 + JPP_EEPROM_PAGE_SIZE];
    frame[0] = (uint8_t)(addr >> 8u);
    frame[1] = (uint8_t)(addr & 0xFFu);
    memcpy(frame + 2, buf, len);

    esp_err_t err = i2c_master_transmit(state->hw_dev, frame, 2u + len,
                                        EEPROM_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write @0x%04X len %u: %s", addr, (unsigned)len,
                 esp_err_to_name(err));
        return JPP_EEPROM_ERR_IO;
    }
    /* Wait out the self-timed write cycle before the next access. */
    return wait_write_complete(state);
}

jpp_eeprom_result_t jpp_eeprom_write(jpp_eeprom_state_t *state,
                                     uint16_t addr, const uint8_t *buf, size_t len)
{
    if (state == NULL || buf == NULL) { return JPP_EEPROM_ERR_INVALID_ARGUMENT; }
    if (!state->present)               { return JPP_EEPROM_ERR_NOT_PRESENT; }
    if (len == 0u)                     { return JPP_EEPROM_OK; }
    if ((size_t)addr + len > state->size_bytes) { return JPP_EEPROM_ERR_OUT_OF_RANGE; }

    size_t off = 0u;
    while (off < len) {
        uint16_t cur      = (uint16_t)(addr + off);
        size_t   page_rem = JPP_EEPROM_PAGE_SIZE - (cur % JPP_EEPROM_PAGE_SIZE);
        size_t   chunk    = len - off;
        if (chunk > page_rem) { chunk = page_rem; }

        jpp_eeprom_result_t rc = write_page(state, cur, buf + off, chunk);
        if (rc != JPP_EEPROM_OK) { return rc; }
        off += chunk;
    }
    return JPP_EEPROM_OK;
}
