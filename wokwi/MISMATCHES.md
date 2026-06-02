# Wokwi Mismatches

## Simulator-valid
These are the behaviors a manual Wokwi session can help demonstrate on the in-repo reference topology:
- native boot sequence through the ESP-IDF application image (`build/jppdos_idf.bin`) with serial markers such as `SYSTEM_READY`
- launcher rendering and recovery-mode behavior
- App discovery and manifest rejection handling when the simulated storage layout matches the scenario being inspected

## Hardware-only checks

| Area | What Wokwi proves | What Wokwi cannot prove | Hardware-only validation |
| --- | --- | --- | --- |
| ADC keypad tolerance | Ladder topology and simulated short/long press behavior | Real resistor variance, voltage drift, and edge-band noise | Sweep the keypad ladder on hardware and record actual UV bands |
| RTC backup and drift | Time-read/write flow and reboot continuity | Crystal drift, backup-battery retention, and long-duration accuracy | Measure drift against a trusted clock and test backup power loss |
| Hard power-cut persistence | Normal settings save/restore flow | Brown-out corruption and last-moment flash write loss | Pull power mid-write on physical hardware and reboot-check recovery |
| Wi-Fi association quality | Broker API and config flow | RF signal quality, roaming, and real AP reliability | Validate against the target access point under real conditions |
| MicroSD contact reliability | App discovery and manifest validation | Card contact bounce, wear, and card-specific timing | Repeat insert/remove and boot cycles with multiple cards |

## Operational rule
Simulator passes are necessary but not sufficient for analog tolerance, RTC accuracy, or power-loss persistence.
