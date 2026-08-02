# HARDWARE_SUMMARY.md — Agent Reference for the ESP32-C6-based J++Device

*Firmware v1.1 · 2026-07-29*

Purpose: give a coding agent everything needed to work on ESP-IDF firmware
for the **J++Device**. This file documents the board's MCU, every
pin, every connected device/IC, the bus configuration each one needs, and the
non-obvious hardware quirks the firmware handles. Pin numbers
and electrical facts match `main/jpp_hw_config.h`
plus the per-peripheral drivers and are authoritative for the physical board.

If you only read one thing: the pin map in the next section plus the
[Hardware quirks](#hardware-quirks-must-handle) section are the parts you cannot
rediscover by reading datasheets — they are board-specific wiring and silicon
behavior.

---

## 1. MCU / SoC

| Property | Value |
|---|---|
| Target | **ESP32-C6** (`CONFIG_IDF_TARGET="esp32c6"`, RISC-V single core) |
| CPU clock | 160 MHz |
| Flash | 2 MB (`CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y`) — a tight budget; see `partitions.csv` and the flash-budget note in AGENTS.md |
| Partition table | `partitions.csv`; `nvs` + `phy_init`, then a maximized `factory` app at offset 0x10000 (0x1D8000 = 1,933,312 B), plus minimal `data_fs`/`runtime_fs` SPIFFS. Coredump-to-flash is disabled, so there is no coredump partition. |
| ESP-IDF | v5.5.1 |
| FreeRTOS tick | 100 Hz (`CONFIG_FREERTOS_HZ=100`) |
| Console | Native USB-Serial-JTAG (logs + JPPD-SMP binary protocol); no separate UART bridge chip on this board, so UART0 is not reachable from a host |
| Main task stack | 8192 bytes (`CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`) |

ESP-IDF logging and the JPPD-SMP binary management protocol (see §7) share the
single native USB-Serial-JTAG peripheral — the board's one USB-C port has no
separate UART bridge chip, so this is also the interface used for flashing and
the serial console. A TX mutex keeps log lines and JPPD-SMP binary frames from
interleaving on the wire.

---

## 2. Complete pin map

All GPIO numbers are fixed by the PCB. Do not reassign without a board respin.

| GPIO | Net / function | Direction | Connected to |
|---|---|---|---|
| **1** | Battery ADC sense | analog in | Li-Po pack via 2×220 kΩ divider (ADC1 ch1) |
| **2** | Keypad ADC | analog in + pullup | 5-key resistor ladder (ADC1 ch2) |
| **3** | Buzzer PWM | output (LEDC) | Passive piezo buzzer |
| **4** | LoRa DIO0 | input + pullup | SX1276 DIO0 (IRQ line; sampled, not used for RF) |
| **5** | LoRa RST | output | SX1276 reset (active-low) |
| **6** | SPI CS1 | output | SX1276 LoRa chip-select |
| **7** | SPI CS2 | output | SD card chip-select |
| **8** | WS2812 data | output (RMT) | Onboard single-pixel addressable RGB LED — the only GPIO not otherwise claimed by this pin map |
| **9** | BOOT strap | input | Boot/download button (standard ESP32-C6 strap) |
| **14** | SPI CLK | output | Shared SPI bus clock (SX1276 + SD) |
| **15** | SPI MOSI | output | Shared SPI bus MOSI |
| **18** | SPI MISO | input | Shared SPI bus MISO |
| **19** | I2C SCL | open-drain | Shared I2C bus (SSD1306 + DS1307 + AT24C32 EEPROM) |
| **20** | I2C SDA | open-drain | Shared I2C bus (SSD1306 + DS1307 + AT24C32 EEPROM) |

USB D+/D- go to the native USB-Serial-JTAG (used for flashing + serial console).

---

## 3. Buses

### I2C — shared, `I2C_NUM_0`
- SCL = GPIO19, SDA = GPIO20.
- **100 kHz** bus speed. This is a hard ceiling: the DS1307 RTC maxes at 100 kHz,
  and both devices must run at the same speed since they share the bus.
- Uses the **new** `driver/i2c_master.h` API (`i2c_new_master_bus`,
  `i2c_master_bus_add_device`, `i2c_master_transmit/_transmit_receive`).
- **Internal pull-ups enabled in firmware** (`flags.enable_internal_pullup=true`,
  `glitch_ignore_cnt=7`). If the board has weak/no external pull-ups, keep this on.
- Three devices on the bus: SSD1306 OLED @ **0x3C**, DS1307 RTC @ **0x68**,
  AT24C32 EEPROM @ **0x50** (piggybacked on the RTC breakout board).
- The OLED is required; the **DS1307 and AT24C32 are both optional**. Each is
  probed at boot (`i2c_master_probe`) and only marked present if it ACKs — a
  board with neither fitted still boots and runs normally (clock-less, no LRV
  identity). See §4.2 and §4.8.

### SPI — shared, `SPI2_HOST`
- MISO=18, MOSI=15, CLK=14. DMA = auto. `max_transfer_sz = 4096`.
- SPI **mode 0** for both devices.
- Two chip-selects on the one bus, each its own `spi_bus_add_device`:
  - SX1276 LoRa: CS=GPIO6, **8 MHz**.
  - SD card: CS=GPIO7, **20 MHz** (added by the SD/FATFS driver, not manually).
- Init order matters: call `spi_bus_initialize(SPI2_HOST, ...)` once, add the
  LoRa device manually, and let `esp_vfs_fat_sdspi_mount` add the SD device on
  the same host later.

### ADC — `ADC_UNIT_1`, oneshot mode
- Two channels in use: ch1 = battery (GPIO1), ch2 = keypad (GPIO2).
- **12 dB attenuation** on both channels (≈0–3.9 V range), default bitwidth (12-bit).
- Calibration: tries **curve-fitting** first, falls back to **line-fitting**,
  falls back to a raw `raw * 3900 / 4095` conversion if neither scheme compiles in.
  (`sdkconfig.defaults` enables the eFuse TP/Vref + LUT calibration options.)

---

## 4. Connected devices / peripherals / ICs

### 4.1 SSD1306 OLED (I2C 0x3C, 128×64)
- Monochrome, 128×64, addressed over the shared I2C bus at 0x3C (note the
  troubleshooting alternate: some modules are strapped to 0x3D).
- Driver keeps a 1024-byte framebuffer `[8 pages][128 cols]`, flushes page-by-page.
- Command byte prefix `0x00` (Co=0, D/C#=0); data prefix `0x40`.
- **Init sequence used (must be replicated):**
  `AE / D5 80 / A8 3F / D3 00 / 40 / 8D 14 / 20 00 / A1 / C8 / DA 12 / 81 CF /
  D9 F1 / DB 40 / A4 / A6 / AF`.
  - `8D 14` = charge pump ON (this module has no external boost — display stays
    blank if you skip it).
  - `A1` + `C8` = segment remap + COM scan flip (panel is mounted so it reads
    correctly only with both remaps; without them the image is mirrored/flipped).
  - `20 00` = horizontal addressing mode; flush sets column range `21 00 7F` and
    page range `22 00 07`.
- Text uses a built-in 5×7 font (`font5x7.h`), 6 px per char → 21 chars/row, 8 rows.

### 4.2 DS1307 RTC (I2C 0x68)
- **Optional.** `jpp_rtc_state_init()` probes the bus and only sets
  `hw_attached` when the chip ACKs; a board with no RTC fitted runs
  clock-less (no periodic hardware reads) rather than failing to boot. Without
  hardware and before any NTP sync, `jpp_rtc_get_current()` returns
  `UNAVAILABLE` and every clock display falls back to `--:--`.
- Standard DS1307, BCD registers starting at 0x00.
- Register 0 bit 7 = **CH (Clock Halt)**. A fresh/dead coin cell ships with CH=1
  (oscillator stopped); writing the seconds register with bit7=0 starts it.
- 24-hour mode assumed (hour register masked with 0x3F).
- Firmware reads 7 bytes (sec,min,hour,wday,day,month,year) and verifies the
  seconds counter advances over ~1.1 s to confirm the crystal is oscillating.
- **Max 100 kHz** — this is what caps the shared I2C bus speed.
- **QUIRK — CH bit set with non-zero registers.** CH=1 is not limited to
  completely-zeroed chips. If the backup coin cell dies or is absent while the
  board is powered, the DS1307 can retain its previous register values (e.g.
  `00:00:00 01/01/00`) but still have CH=1, so the oscillator is halted even
  though the time registers look plausible. Checking for all-zero registers is
  not a reliable proxy for "oscillator stopped"; the CH bit must be read and
  cleared explicitly. Use a targeted read-modify-write on register 0 (`sec_raw
  & 0x7F`) rather than writing a full default time, so the other registers are
  not clobbered.

### 4.3 SX1276 LoRa transceiver (SPI, 868 MHz band)
- SPI mode 0, 8 MHz, CS=GPIO6. RST=GPIO5 (active low), DIO0=GPIO4.
- Register access: read = address with MSB cleared; write = address with MSB set
  (`reg | 0x80`); 16-bit transactions (1 addr byte + 1 data byte).
- **Reset pulse:** drive RST low 10 ms, release, wait 10 ms before first access.
- Identity check: version register **0x42 must read 0x12**.
- Configured for the **868 MHz EU band** (firmware programs Frf for 868.1 MHz:
  `Frf = freq / (32e6 / 2^19)`, 32 MHz reference crystal). The board's matching
  network / antenna is therefore an 868 MHz design — do not assume 433/915.
- DIO0 is wired (input-with-pullup); a driver that services the radio maps
  DIO0 to RxDone/TxDone. JPPDOS reserves the LoRa CS (GPIO6) on the shared SPI
  bus but does not drive the SX1276.

### 4.4 SD card (SPI mode, FATFS)
- SPI mode, CS=GPIO7, up to 20 MHz, shares the SX1276 SPI bus (SPI2_HOST).
- Mounted via `esp_vfs_fat_sdspi_mount` at `/sd`, FAT32 only
  (exFAT/NTFS not supported), no auto-format; mounted at boot and held
  mounted (`mount_sd()` in `main/jpp_hw_init.c`).
- **CRITICAL QUIRK — patched sdmmc component:** the repo ships
  `components/sdmmc/` which overrides ESP-IDF's stock `sdmmc` component. The only
  changed file is `sdmmc_sd.c`, patched to make **CMD59 (CRC_ON_OFF) failure
  non-fatal**. Some cards on this board don't implement CMD59 even though the
  SPI-mode spec requires it; stock IDF aborts the mount, so the override lets
  those cards mount (same approach Flipper Zero / Arduino SD take). **A new
  firmware must carry this same override or those cards won't mount.** All other
  sdmmc sources are pulled straight from `$IDF_PATH/components/sdmmc`.
- Hardware note: some cards need a 10 µF cap on the 3.3 V rail to
  mount reliably.

### 4.5 Battery sense (ADC1 ch1 / GPIO1)
- Li-Po pack through **two equal 220 kΩ resistors** = 1:1 divider, so
  `Vbat = Vpin × 2` (`BATT_DIV_FACTOR = 2`).
- 12-bit ADC, 12 dB atten, firmware averages **8 samples**.
- Pin voltage is converted from the raw ADC count via the ESP-IDF ADC
  calibration driver (`adc_cali_create_scheme_curve_fitting` /
  `adc_cali_raw_to_voltage`, set up once in `jpp_battery_cali_init()`), not a
  flat `raw × 3.3 V / 4095` formula — the ESP32-C6 ADC is non-linear enough at
  12 dB atten that the flat formula alone reads a full battery well below its
  real voltage. If the calibration scheme can't be created (missing eFuse
  calibration bits), `jpp_battery_read()` falls back to the flat formula.
- SoC mapping: 4200 mV = 100 %, 3000 mV = 0 %, linear between.
- Healthy window: 3000–4400 mV.

### 4.6 Keypad — 5-key resistor ladder (ADC1 ch2 / GPIO2)
- Five buttons share GPIO2 through a resistor ladder. The **ESP32-C6 internal
  ~45 kΩ pull-up is the top of the divider** — firmware enables it
  (`gpio_pullup_en`) and without it the pin floats and ghost-presses.
- 12-bit ADC, 12 dB atten. Calibrated raw bands (reference unit):

  | Key | Raw centre | Threshold (raw <) |
  |---|---|---|
  | LEFT (≈0 Ω short) | ~4 | 250 |
  | UP (≈5.7 kΩ) | ~492 | 806 |
  | DOWN (≈15.5 kΩ) | ~1120 | 1412 |
  | RIGHT (≈29 kΩ) | ~1710 | 2094 |
  | SELECT (≈59 kΩ) | ~2476 | 2910 (NONE threshold) |
  | NONE (ladder bleed, ≈148 kΩ) | ~3344 | raw ≥ 2910 |

- Pull-up resistance varies ±30 % chip-to-chip, so thresholds are midpoints
  between adjacent bands and may need re-tuning per unit.
- Debounce in firmware: takes two reads 10 ms apart, rejects if they differ by
  >50 counts; press confirmed by a second matching read 30 ms later.

### 4.7 Buzzer (passive piezo, GPIO3, LEDC PWM)
- **Passive** buzzer → must be driven with a PWM tone, not a DC level. Uses LEDC
  `LEDC_TIMER_0` / `LEDC_CHANNEL_0`, low-speed mode, 10-bit resolution, 50 % duty
  (512/1024). Change `ledc_set_freq` per note.
- **QUIRK:** GPIO3 is an LP (low-power) GPIO pad with low default drive (~5 mA).
  Firmware calls `gpio_set_drive_capability(GPIO_NUM_3, GPIO_DRIVE_CAP_3)` to get
  full voltage swing / adequate loudness. Replicate this.
- Polarity-insensitive (passive piezo).

### 4.8 AT24C32 EEPROM (I2C 0x50, on the RTC breakout)
- **Optional**, like the DS1307 — `jpp_eeprom_state_init()` probes the bus and
  only marks the chip `present` when it ACKs.
- 4096 bytes (32 Kbit), 2-byte big-endian word address, 32-byte page-write
  boundary, ~5 ms self-timed write cycle per page. The driver polls for ACK
  rather than a fixed delay (`components/jpp_core/src/jpp_eeprom_core.c`) —
  at `FREERTOS_HZ=100`, `pdMS_TO_TICKS(6)` rounds down to 0 ticks, so any
  sub-10 ms `vTaskDelay` here would be a silent no-op and the next page write
  would NACK.
- Backs LRV (Limited Run Verification) identity storage: a write-once record
  written at manufacturing time, read straight into RAM at boot
  (`jpp_lrv_init()`). Stored raw/unencrypted — no password, no unlock step.
  Because the chip is external to the ESP32-C6, the identity survives factory
  reset and a full firmware reflash.

### 4.9 Onboard WS2812 LED (GPIO8, RMT)
- Single-pixel addressable RGB LED, driven by the RMT TX peripheral with a
  hand-rolled bit encoder — no `led_strip` managed-component dependency, to
  keep flash footprint minimal (see the 2 MB flash budget in §1).
- GPIO8 is otherwise unused by this pin map, making it the only free GPIO on
  the board.
- Ungated App SDK surface: `jpp_sdk_led_set_color(ctx, r, g, b)` /
  `jpp_sdk_led_off(ctx)`.

---

## 5. Hardware quirks (must handle)

These are the board/silicon specifics that are easy to get wrong:

1. **Shared I2C bus lockup recovery.** A failed DS1307 transaction can leave SDA
   held low, which blocks *every* subsequent transaction on the shared bus
   (including the OLED). After any DS1307 failure the firmware calls
   `i2c_master_bus_reset()` to pulse SCL and free the bus. Build the same
   recovery path or one bad RTC read can freeze the display.
2. **DS1307 CH bit set with valid-looking registers.** The oscillator-halt bit
   (reg 0 bit 7) can be set even when the time registers hold non-zero values
   (observed: `00:00:00 01/01/00` with sec unchanging after 1.1 s). Do not
   rely on all-zeros detection to decide whether to restart the oscillator;
   always clear the CH bit explicitly with a read-modify-write on register 0
   before the ticking check. Handled in `jpp_rtc_hw_read()`
   (`components/jpp_core/src/jpp_rtc_core.c`).
3. **SD card CMD59 override** (see §4.4) — ship the patched `components/sdmmc/`.
4. **OLED charge pump** (`8D 14`) and **segment/COM remap** (`A1`,`C8`) are
   mandatory for this panel; omitting them gives a blank or mirrored screen.
5. **Keypad internal pull-up** is a circuit element, not just an input config —
   the ladder math assumes ~45 kΩ to VCC inside the chip.
6. **Buzzer drive strength boost** on GPIO3 (LP pad).
7. **I2C capped at 100 kHz** by the DS1307; don't raise it for the OLED.
8. **SX1276 reset timing** (10 ms low / 10 ms settle) before first SPI access.
9. **868 MHz** RF front-end — band is fixed by the board's matching network.
10. **ADC 12 dB attenuation** on both analog channels; calibration scheme is
    best-effort with graceful fallback.

---

## 6. Initialization order

Orchestrated by `app_main()` in `main/app_main.c`, which calls into the split
boot modules (`jpp_hw_init.c`, `jpp_boot_display.c`, `jpp_wifi_init.c`,
`jpp_native_services.c`, `jpp_app_dispatch.c` — see AGENTS.md's "main/ module
split" note):

```
jpp_heap_monitor_init()   // failed-alloc logging + low-heap sampler, first thing
jpp_buzzer_init()         // LEDC timer+channel, drive-strength boost on GPIO3
init_i2c()                // master bus @100kHz; OLED(0x3C) probed/init'd for boot splash
mount_flash_storage()     // /data + /lib SPIFFS mounts
settings load             // /data/settings.json probe / defaults
mount_sd()                // SPI2 bus + SD card at /sd, held mounted
jpp_rtc_state_init()      // probes DS1307 @0x68; optional, hw_attached only if it ACKs
jpp_lrv_init()             // probes/binds AT24C32 EEPROM @0x50 on the same I2C bus; optional
init_wifi()                // NVS + esp_wifi STA mode
jpp_ble_native_init()      // NimBLE controller + host bring-up
jpp_native_services_init() // wires broker callbacks (file/HTTP/KV/RTC/BLE/ESP-NOW)
jpp_serial_mgr_init()      // USB-Serial-JTAG driver install + TX log mutex (JPPD-SMP)
app discovery              // /sd/apps scan, launcher handoff
```

---

## 7. Serial behavior
- ESP-IDF logs go out over the native **USB-Serial-JTAG** peripheral (not
  UART0 — this board's single USB-C port has no separate UART bridge chip, so
  UART0 is unreachable from a host).
- The same peripheral carries **JPPD-SMP**, the binary management protocol
  (`main/jpp_serial_mgr.c`, installed via `usb_serial_jtag_driver_install`): a
  host PC can manage SD files, query device info, and retrieve LRV data after
  the user approves the session on-device. A TX mutex (registered over the
  ESP_LOG vprintf sink) keeps log lines and binary frames from interleaving.
  There is no baud rate to configure — it's a USB CDC-ACM byte stream.

---

## 8. Required ESP-IDF components
From `main/CMakeLists.txt`: `jpp_core`, `jpp_native_loader_core`,
`jpp_crypto_core`, `spiffs`, `fatfs`, `sdmmc` (overridden, see §4.4),
`driver`, `esp_driver_usb_serial_jtag`, `esp_adc`, `json`, `esp_wifi`,
`esp_netif`, `lwip`, `nvs_flash`, `bt`, `esp_http_client`,
`esp_timer`, `esp_rom`, `espressif__libsodium`, `mbedtls`, and `esp-tls`.
`esp_http_server` is deliberately **not** in the list: the WebDAV and LRV
servers run on the in-house `jpp_http_server_core` so their memory comes from
the app pool rather than the heap the Wi-Fi driver shares (see `AGENTS.md`).
