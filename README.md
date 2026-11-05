# EegoRead A4 Developer Demo

Private standalone ESP32-S3 firmware for EegoRead A4 hardware demonstrations.
The project starts directly into a touch-driven developer menu and contains
only board, display, storage, radio-scan and demo-page code.

## Pages

- Overview: model, power-board profile, firmware, MAC, battery, RTC and SD
- Display / Refresh: GC, DU, measured timing and test patterns
- Touch: GSL1680 coordinates and a live 24-point trail
- Hardware Keys: live UP/DOWN/POWER levels and press counts
- Frontlight: independent cold/warm sliders for LM3630A
- Power / Battery: CW2015 or legacy ADC, charge and GPIO profile
- RTC: PCF8563 read-only date/time
- microSD: mount and read-only root listing
- Wi-Fi Scan: scan-only radio demonstration
- BLE Observer: NimBLE scan-only demonstration

## Hardware

- ESP32-S3, 16 MiB flash and 8 MiB octal PSRAM
- UC8279C-compatible 768 x 552 electronic-paper panel
- GSL1680 touch controller
- LM3630A frontlight on A4; absent on A4 Lite
- CW2015 or legacy calibrated ADC battery path
- PCF8563 RTC and SPI microSD

The UI framebuffer is portrait 552 x 768, 1bpp, then rotated to the panel
raster before refresh.

## Build

Requires ESP-IDF 6.0.1.

```sh
source scripts/idf_env.sh
idf.py -B build-final set-target esp32s3
idf.py -B build-final build
```

For later incremental builds:

```sh
source scripts/idf_env.sh
idf.py -B build-final build
```

## Flash

The verified app-only script writes `build-final/eegoread_firmware.bin` to app0
at `0x10000` using the esptool stub:

```sh
bash scripts/flash_app_verified.sh /dev/cu.usbmodem1101
```

If OTA metadata currently selects app1, use the target's normal recovery or
explicitly clear only the OTA-selection region `0xe000:0x2000` before booting
the demo from app0.

## Boundaries

This repository does not contain or initialize:

- reader/session/EPUB/TXT implementation;
- OTA, cloud, account or sync services;
- Wi-Fi/BLE file transfer;
- USB MSC;
- automatic lock/sleep or the production reader entry point.

Wi-Fi and BLE pages are scan-only. Each radio is stopped when the page exits.
No Wi-Fi credentials are read or written, and BLE does not create GATT services,
connect, pair or bond.

See [Developer Demo Guide](docs/DEVELOPER_DEMO.md) for pin maps, hardware
identification, driver APIs and verification boundaries.

## Licensing

The generated demo font subset retains the applicable font notices under
`LICENSES/fonts/`.
