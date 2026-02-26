# ESP32-133C02 Spectra 6 Firmware (13.3-inch)

This project provides adapted firmware for the **13.3-inch E-Ink Spectra 6 Display** paired with the **ESP32-133C02** driver board, originally ported from [shi-314/esp32-spectra-e6](https://github.com/shi-314/esp32-spectra-e6).

## Features
- Full support for the ESP32-133C02 dedicated display driver board.
- Dual-IC QSPI communication to handle the large 1200x1600 resolution.
- Native rendering of 6 colors (Black, White, Yellow, Red, Blue, Green).
- Configuration Portal for WiFi and Image URL setup.
- Automatic image dithering and downloading from a configurable server.
- Deep Sleep power-saving features specifically optimized for the 13.3" panel.

## Hardware
- **Display Board:** ESP32-133C02
- **Panel:** GDEP133C02 (13.3" Spectra 6 E-Paper)
- **Controller:** ESP32-S3 (with PSRAM)

## Setup & Deployment
1. **Prerequisites:** 
   - [PlatformIO](https://platformio.org/) installed in your IDE (e.g., VSCode).
2. **Build and Upload:**
   - Clone this repository.
   - Run `pio run --target upload` to flash the firmware.
   - Run `pio run --target uploadfs` to upload the SPIFFS filesystem (required for the configuration portal web server).
   
## Configuration
When the device fails to connect to WiFi (or boots for the first time), it enters Configuration Mode:
1. A QR code and WiFi network name (`Framey-Config`) will appear on the display.
2. Connect to the network.
3. Automatically opens the configuration portal where you can set your WiFi credentials and image URL.

## Architecture Notes
The original project was built using the `GxEPD2` library, which only handles standard SPI. Because the 13.3" panel leverages a dedicated QSPI interface via dual-ICs, a `DisplayAdapter` was created to encapsulate the low-level manufacturer C driver into an `Adafruit_GFX` compatible class.

## License
MIT License. Original base logic by shi-314. Adapted by dandwhelan.
