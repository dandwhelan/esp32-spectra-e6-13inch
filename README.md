# ESP32-133C02 — 13.3" E-Ink Spectra 6 Display Firmware

Custom firmware for the **Good-Display 13.3-inch E-Ink Spectra 6 panel (GDEP133C02)** paired with the **ESP32-133C02** driver board. Displays full-color (6-colour) images uploaded via a built-in web portal, then enters deep sleep to preserve the image indefinitely.

Originally adapted from [shi-314/esp32-spectra-e6](https://github.com/shi-314/esp32-spectra-e6) (MIT License), which targeted a smaller display. This fork adds full support for the 13.3" panel's dual-IC QSPI interface, image upload via a web server, Floyd-Steinberg dithering, JPEG/PNG/BMP decoding, automatic image scaling, and deep sleep management.

---

## Table of Contents

- [Features](#features)
- [Hardware](#hardware)
- [Pin Mapping](#pin-mapping)
- [SD Card Pins](#sd-card-pins--shared-spi-bus)
- [Architecture Overview](#architecture-overview)
- [Boot Flow](#boot-flow)
- [Image Processing Pipeline](#image-processing-pipeline)
- [Setup & Deployment](#setup--deployment)
- [Configuration](#configuration)
- [Flash Partition Layout](#flash-partition-layout)
- [Dependencies & Libraries](#dependencies--libraries)
- [Source Code Structure](#source-code-structure)
- [Memory Management](#memory-management)
- [License](#license)

---

## Features

- **6-colour rendering** — Black, White, Yellow, Red, Blue, Green via the Spectra 6 palette.
- **Web-based image upload** — Upload JPEG, PNG, or BMP images through a browser; the display refreshes immediately.
- **Floyd-Steinberg dithering** — Automatic colour quantization with error diffusion for natural-looking images on the limited 6-colour palette.
- **Automatic image scaling** — Images smaller than 1200×1600 are upscaled via nearest-neighbour interpolation. Images larger than 1200×1600 are cropped to fit.
- **Deep sleep** — After a 10-minute web server window, the device enters permanent deep sleep. Only a hardware power reset wakes it.
- **PSRAM-optimised** — All large buffers (framebuffer, decode buffers, dither bitmaps) are allocated in the ESP32-S3's 8 MB PSRAM.
- **Dual-IC QSPI** — Custom `DisplayAdapter` bridges the manufacturer's C driver into an `Adafruit_GFX`-compatible API, handling the split framebuffer across two driver ICs.
- **WiFi configuration portal** — First-boot Access Point mode with a web UI for entering WiFi credentials and an image URL.
- **NVS persistent storage** — WiFi credentials and image URL survive deep sleep and power cycles.
- **LittleFS image storage** — Uploaded images are stored on the internal flash filesystem (~5.6 MB partition).


---

## Hardware

| Component | Details |
|---|---|
| **Driver Board** | ESP32-133C02 (Good-Display) |
| **MCU** | ESP32-S3-WROOM-1 (N16R8) — 16 MB Flash, 8 MB PSRAM |
| **Display Panel** | GDEP133C02 — 13.3" E-Ink Spectra 6 |
| **Resolution** | 1200 × 1600 pixels |
| **Colours** | 6 (Black, White, Yellow, Red, Blue, Green) |
| **Interface** | QSPI with dual driver ICs (CS0 + CS1) |
| **Power** | USB-C or battery (with brownout detection) |

---

## Pin Mapping

### Display (QSPI)

The display uses a **Quad-SPI** interface with **two chip-select lines** (one per driver IC — each IC handles half the display width).

| Function | GPIO | Direction | Notes |
|---|---|---|---|
| `SPI_CLK` | **9** | Output | SPI clock |
| `SPI_Data0` | **41** | Bidirectional | QSPI data line 0 |
| `SPI_Data1` | **40** | Bidirectional | QSPI data line 1 |
| `SPI_Data2` | **39** | Bidirectional | QSPI data line 2 |
| `SPI_Data3` | **38** | Bidirectional | QSPI data line 3 |
| `SPI_CS0` | **18** | Output | Chip select — left half (driver IC 0) |
| `SPI_CS1` | **17** | Output | Chip select — right half (driver IC 1) |
| `EPD_BUSY` | **7** | Input | Display busy signal |
| `EPD_RST` | **6** | Output | Display hardware reset |
| `LOAD_SW` | **45** | Output | Load switch (power to display) |

### Switches (optional, currently unused in firmware)

| Function | GPIO | Notes |
|---|---|---|
| `SW_2` | **13** | External pull-down on board |
| `SW_4` | **21** | User button |

---

## SD Card Pins & Shared SPI Bus

> **Important:** The SD card slot on the ESP32-133C02 board shares the **same SPI data and clock lines** as the E-Ink display. This is a critical design detail.

### SD Card Pin Mapping

| Function | GPIO | Shared With |
|---|---|---|
| **SD_CLK** (SCK) | **9** | `SPI_CLK` (display) |
| **SD_CMD** (MOSI) | **41** | `SPI_Data0` (display) |
| **SD_D0** (MISO) | **40** | `SPI_Data1` (display) |
| **SD_CS** | **21** | `SW_4` (user button) |

### Why the Display and SD Card Share Pins

The ESP32-133C02 board connects both the display and the SD card to the same `SPI3_HOST` bus. They are differentiated by their **chip select (CS) lines**:

- **CS0 (GPIO 18)** → Display driver IC 0 (left half)
- **CS1 (GPIO 17)** → Display driver IC 1 (right half)
- **CS (GPIO 21)** → SD card

This means the display and SD card **cannot be used simultaneously**. The firmware must ensure the display's CS lines are **deasserted (HIGH)** before accessing the SD card, and vice versa. In the current firmware, SD card support is not enabled — images are stored on the internal LittleFS flash filesystem instead.

> **Note:** If you want to add SD card support in the future, you would initialize `SD.begin(21)` using `SPI3_HOST` (the same bus), being careful to manage chip-select timing to avoid bus conflicts.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        main.cpp (setup)                        │
│  Boot → WiFi → Display Image → Web Server (10 min) → Sleep    │
├─────────────┬───────────────┬───────────────┬──────────────────┤
│ ImageScreen │ ConfigScreen  │ ConfigServer  │ WiFiConnection   │
│  (render)   │   (AP mode)   │ (web portal)  │  (STA connect)   │
├─────────────┴───────────────┴───────────────┴──────────────────┤
│                     DisplayAdapter                              │
│        Adafruit_GFX subclass → PSRAM framebuffer               │
│      Handles rotation, colour packing, dual-IC split           │
├─────────────────────────────────────────────────────────────────┤
│              Manufacturer C Driver Layer                        │
│   GDEP133C02.c  +  comm.c  +  pindefine.h                     │
│   QSPI init, GPIO config, EPD commands, power sequencing       │
├─────────────────────────────────────────────────────────────────┤
│                    ESP32-S3 Hardware                            │
│           SPI3_HOST  •  8MB PSRAM  •  16MB Flash                │
└─────────────────────────────────────────────────────────────────┘
```

### Key Design Decision: DisplayAdapter

The original project by [shi-314](https://github.com/shi-314/esp32-spectra-e6) used the `GxEPD2` library, which only supports standard SPI. The 13.3" panel requires **QSPI with dual driver ICs**, so a custom `DisplayAdapter` class was created:

1. **Inherits from `Adafruit_GFX`** — provides `drawPixel()`, `fillScreen()`, `drawBitmap()`, text rendering, etc.
2. **PSRAM framebuffer** — a 960 KB buffer (1200 × 1600 pixels, 4 bits per pixel, 2 pixels packed per byte).
3. **Dual-IC split** — on `display()`, the framebuffer is sent row-by-row: left 600px to CS0, right 600px to CS1.
4. **Colour mapping** — uses 4-bit colour codes matching the manufacturer's constants (e.g., `BLACK = 0x00`, `WHITE = 0x11`, `RED = 0x33`).

---

## Boot Flow

```
Power On / Reset
      │
      ▼
  Serial.begin(115200)
  Load config from NVS
      │
      ├── Has WiFi credentials?
      │       │
      │       ├── YES → Connect to WiFi
      │       │         │
      │       │         ▼
      │       │    Display stored image immediately
      │       │         │
      │       │         ▼
      │       │    Start web server (port 80)
      │       │    Run for 10 minutes
      │       │    (auto-refresh display on new upload)
      │       │         │
      │       │         ▼
      │       │    Enter permanent deep sleep
      │       │
      │       └── NO → Display stored image
      │                Start Access Point ("Framey-Config")
      │                Run config web portal for 10 minutes
      │                Enter permanent deep sleep
      │
      ▼
  Only a HARDWARE POWER RESET wakes the device
```

---

## Image Processing Pipeline

When an image is uploaded or loaded from LittleFS:

```
Raw Image File (JPEG / PNG / BMP)
      │
      ▼
  Format Detection (magic bytes: FFD8=JPEG, 89PNG=PNG, BM=BMP)
      │
      ├── JPEG: TJpg_Decoder → RGB565 buffer (1200×1600 in PSRAM)
      ├── PNG:  PNGdec → RGB565 buffer (streaming from LittleFS)
      └── BMP:  Manual parser → RGB565 buffer
      │
      ▼
  Scale-to-Fit (if image < 1200×1600)
  Nearest-neighbour upscaling, aspect-ratio preserved, centred
      │
      ▼
  Floyd-Steinberg Dithering
  Quantise each pixel to nearest of 6 Spectra colours
  Diffuse error to neighbouring pixels (7/16, 3/16, 5/16, 1/16)
      │
      ▼
  Generate 5 colour bitmaps (1-bit each):
  Black, Yellow, Red, Blue, Green  (White = no bits set)
      │
      ▼
  Render bitmaps via DisplayAdapter::drawBitmap()
  Pack into 4-bit PSRAM framebuffer
      │
      ▼
  Send to display via QSPI (dual-IC split)
  Trigger e-paper refresh (~20 seconds)
```

### Supported Image Formats

| Format | Max Size | Notes |
|---|---|---|
| **JPEG** | Any (decoded in tiles) | Source buffer freed before dithering to save PSRAM |
| **PNG** | Any (streamed line-by-line from LittleFS) | Supports alpha channel |
| **BMP** | 24-bit uncompressed | Directly parsed, no library needed |

### Screen Sizes & Display Dimensions

| Property | Value |
|---|---|
| **Physical screen** | 13.3 inches diagonal |
| **Native resolution** | 1200 × 1600 pixels |
| **Orientation** | Portrait (width < height) |
| **Aspect ratio** | 3:4 |
| **Pixel density** | ~150 PPI |
| **Colour depth** | 6 colours (Black, White, Yellow, Red, Blue, Green) |
| **Refresh time** | ~20 seconds for a full screen update |

The ideal source image is **1200 × 1600 pixels** in portrait orientation. At this exact size, the image maps 1:1 to the display with no scaling or cropping.

### How Image Resizing Works

The firmware handles images of any size — it will never reject an upload. Here's what happens depending on the uploaded image dimensions:

| Uploaded Image | What Happens |
|---|---|
| **Exactly 1200×1600** | Pixel-perfect — no scaling, no cropping |
| **Smaller than 1200×1600** | **Upscaled** via nearest-neighbour interpolation to fill the display |
| **Larger than 1200×1600** | **Cropped** to the top-left 1200×1600 pixels (no downscaling) |
| **Different aspect ratio** | Scaled to fit within 1200×1600 while preserving aspect ratio, centred on a white background |

**Upscaling detail (small images):**

The `scaleToFit()` function in `ImageScreen.cpp` performs nearest-neighbour upscaling:

1. Calculate the scale factor: `scale = min(1200/srcWidth, 1600/srcHeight)`
2. Compute the scaled dimensions while maintaining the original aspect ratio
3. Centre the scaled image in the 1200×1600 buffer (white bars on any unused edges)
4. For each destination pixel, map back to the nearest source pixel — no blurring or interpolation artefacts

For example, a **450×800** image (from a web thumbnail) would be scaled by a factor of **2×**, producing a **900×1600** image centred horizontally with 150px white bars on each side.

> **Tip:** For the best quality, always upload images at **1200×1600**. Upscaling a small image will show visible pixelation. Use any image editor (or an online tool) to resize and crop your photos beforehand.

### How Floyd-Steinberg Dithering Works

E-Ink Spectra 6 displays can only show **6 discrete colours**. A typical photograph contains millions of colours, so the firmware must **quantise** every pixel to one of the 6 available colours. Naïve quantisation (just picking the closest colour) produces harsh, banded transitions. **Floyd-Steinberg error diffusion** dramatically improves this by spreading the "error" (the difference between the original colour and the chosen colour) to neighbouring pixels.

**The Spectra 6 Palette (RGB values):**

| Index | Colour | RGB |
|---|---|---|
| 0 | Black | `(0, 0, 0)` |
| 1 | White | `(255, 255, 255)` |
| 2 | Yellow | `(230, 230, 0)` |
| 3 | Red | `(204, 0, 0)` |
| 4 | Blue | `(0, 51, 204)` |
| 5 | Green | `(0, 204, 0)` |

**The algorithm (per pixel):**

1. Take the current pixel's RGB value, plus any accumulated error from previously processed neighbours
2. Find the **nearest palette colour** using Euclidean distance in RGB space: `distance = dr² + dg² + db²`
3. Calculate the **error** = original RGB − chosen palette RGB
4. Distribute this error to 4 neighbouring pixels:

```
             current pixel    →  right pixel  (+7/16 of error)
                  ↓
  bottom-left  (+3/16)     bottom (+5/16)     bottom-right (+1/16)
```

5. This error diffusion creates a natural-looking pattern of dots from the limited palette, simulating intermediate colours through spatial mixing

**The output** is 5 one-bit bitmaps (one for each non-white colour). A pixel set in the black bitmap gets drawn as black; a pixel set in the yellow bitmap gets drawn as yellow; no bits set = white. These bitmaps are rendered via `Adafruit_GFX::drawBitmap()` and packed into the 4-bit PSRAM framebuffer for QSPI transfer to the display.

---

## Setup & Deployment

### Prerequisites

- [PlatformIO](https://platformio.org/) installed (e.g., VSCode extension)
- ESP32-133C02 board connected via USB-C

### Build & Flash

```bash
# Clone the repository
git clone https://github.com/dandwhelan/esp32-spectra-e6-13inch.git
cd esp32-spectra-e6-13inch

# Build and upload firmware
pio run --target upload

# Upload the LittleFS filesystem (HTML config page)
pio run --target uploadfs

# Monitor serial output
pio device monitor -b 115200
```

### Optional: Custom WiFi Defaults

Create `src/config_dev.h` (gitignored) to set default WiFi credentials for development:

```cpp
#ifndef CONFIG_DEV_H
#define CONFIG_DEV_H

const char DEFAULT_WIFI_SSID[] = "YourNetwork";
const char DEFAULT_WIFI_PASSWORD[] = "YourPassword";
const char DEFAULT_IMAGE_URL[] = "https://example.com/image.png";

#endif
```

---

## Configuration

### First Boot (No Credentials)

1. The device creates a WiFi Access Point: **`Framey-Config`** (password: `configure123`)
2. Connect to it with your phone/laptop
3. Navigate to `http://192.168.4.1`
4. Enter your WiFi SSID, password, and (optionally) an image URL
5. Save — credentials are stored in NVS and persist across reboots

### Normal Operation

1. Device boots and connects to your WiFi
2. Displays the stored image immediately
3. Web server runs for **10 minutes** at the device's IP address (shown in serial monitor)
4. Upload images via the web portal at `http://<device-ip>`
5. Display refreshes automatically after each upload
6. After 10 minutes, device enters **permanent deep sleep** (image stays on screen)
7. **Power reset** (unplug and replug, or press the reset button) to restart the cycle

---

## Flash Partition Layout

| Partition | Type | Offset | Size | Purpose |
|---|---|---|---|---|
| `nvs` | data (nvs) | 0x9000 | 20 KB | WiFi credentials, image URL |
| `otadata` | data (ota) | 0xE000 | 8 KB | OTA metadata |
| `app0` | app (ota_0) | 0x10000 | 2.3 MB | Firmware |
| `spiffs` | data (spiffs) | 0x260000 | **5.6 MB** | LittleFS (uploaded images + HTML) |

The 5.6 MB LittleFS partition is large enough for one high-resolution image at a time.

---

## Dependencies & Libraries

All dependencies are managed by PlatformIO and declared in `platformio.ini`.

### Hardware/Display Libraries

| Library | Source | Purpose |
|---|---|---|
| **Adafruit GFX** | `adafruit/Adafruit GFX Library@^1.11.5` | Base graphics primitives (inherited by DisplayAdapter) |
| **U8g2** | `olikraus/U8g2@^2.36.5` | Font rendering engine |
| **U8g2_for_Adafruit_GFX** | [GitHub](https://github.com/olikraus/U8g2_for_Adafruit_GFX.git) | Bridge between U8g2 fonts and Adafruit_GFX |

### Image Decoding Libraries

| Library | Source | Purpose |
|---|---|---|
| **TJpg_Decoder** | `bodmer/TJpg_Decoder@^1.1.0` | Hardware-accelerated JPEG decoding |
| **PNGdec** | `bitbank2/PNGdec@^1.0.1` | PNG decoding (streaming from file) |

### Networking Libraries

| Library | Source | Purpose |
|---|---|---|
| **ESPAsyncWebServer** | [GitHub](https://github.com/ESP32Async/ESPAsyncWebServer.git) | Async HTTP server for config portal and image uploads |
| **AsyncTCP** | [GitHub](https://github.com/ESP32Async/AsyncTCP.git) | Async TCP layer (required by ESPAsyncWebServer) |
| **WiFi** | ESP32 Arduino core | WiFi STA and AP mode |
| **HTTPClient** | ESP32 Arduino core | Download images from a URL |
| **DNSServer** | ESP32 Arduino core | Captive portal in AP mode |

### Utility Libraries

| Library | Source | Purpose |
|---|---|---|
| **qrcode** | `ricmoo/qrcode@^0.0.1` | QR code generation for config screen |
| **FS / SPIFFS / LittleFS** | ESP32 Arduino core | Filesystem for image storage |

### Upstream / Example Code

| Repository | Relationship |
|---|---|
| [shi-314/esp32-spectra-e6](https://github.com/shi-314/esp32-spectra-e6) | **Original project** — firmware for smaller Spectra 6 displays using GxEPD2. This repo forked the image processing, dithering, WiFi setup, and config portal logic. |
| [Good-Display example code](https://www.good-display.com/) | **Manufacturer C driver** — `GDEP133C02.c`, `comm.c`, `pindefine.h` are adapted from Good-Display's official ESP-IDF example for the ESP32-133C02 board. These handle QSPI initialisation, EPD command sequences, and dual-IC communication. |

---

## Source Code Structure

```
src/
├── main.cpp                  # Entry point: boot flow, WiFi, web server loop, deep sleep
│
├── DisplayAdapter.cpp/.h     # Adafruit_GFX subclass wrapping the QSPI driver
│                               Manages PSRAM framebuffer and dual-IC split transfer
│
├── ImageScreen.cpp/.h        # Image loading, decoding (JPEG/PNG/BMP), 
│                               Floyd-Steinberg dithering, nearest-neighbour scaling,
│                               bitmap rendering to display
│
├── ConfigurationServer.cpp/.h # Async web server: config portal, image upload handler
├── ConfigurationScreen.cpp/.h # AP mode display (QR code, connection info)
│
├── WiFiConnection.cpp/.h     # WiFi STA connection manager (DHCP)
├── HttpDownloader.cpp/.h     # HTTPS image downloader with ETag caching
│
├── ApplicationConfig.h       # Runtime config struct (SSID, password, image URL)
├── ApplicationConfigStorage  # NVS read/write for persistent config
├── config_default.h          # Default empty credentials (safe to commit)
│
├── GDEP133C02.c/.h           # [Manufacturer] EPD init, command sequences, display refresh
├── comm.c/.h                 # [Manufacturer] SPI bus init, GPIO config, SPI transactions
├── pindefine.h               # [Manufacturer] GPIO pin assignments
├── status.h                  # [Manufacturer] Debug logging flag
│
├── battery.cpp/.h            # Battery voltage reading (ADC)
└── Screen.h                  # Abstract screen interface

data/
└── config.html               # Web portal HTML (uploaded to LittleFS via uploadfs)

platformio.ini                # Build config, library dependencies, partition table
partitions.csv                # Custom flash partition layout (5.6 MB for LittleFS)
```

---

## Memory Management

The ESP32-S3 has **8 MB PSRAM** which is critical for image processing:

| Buffer | Size | Purpose |
|---|---|---|
| Display framebuffer | 960 KB | 4-bit packed, 2 pixels/byte |
| RGB565 decode buffer | 3.84 MB | 1200×1600×2 bytes |
| Dither output bitmaps (×5) | 1.2 MB | 1-bit per pixel, per colour |
| Source image data | Variable | Freed after decode to reclaim PSRAM |

### PSRAM Optimisation

For large JPEG files (e.g., 3+ MB from a phone camera), the raw source data is freed **immediately after decoding** but **before dithering** begins. This prevents out-of-memory errors when the source file + decode buffer + dither bitmaps would otherwise exceed 8 MB.

---

## License

MIT License. Original base logic by [shi-314](https://github.com/shi-314/esp32-spectra-e6). Adapted and extended by [dandwhelan](https://github.com/dandwhelan).
