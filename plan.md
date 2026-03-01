# Plan: SD Card Pin Sharing — Approaches to Overcome the Conflict

## The Problem

The ESP32-133C02 board physically wires the SD card slot and the 13.3" e-ink display to the **same SPI bus** (`SPI3_HOST`):

| Signal       | GPIO | Display Use    | SD Card Use |
|-------------|------|----------------|-------------|
| `SPI_CLK`   | 9    | SPI clock      | SD_CLK      |
| `SPI_Data0` | 41   | Data0 (MOSI)   | SD_CMD      |
| `SPI_Data1` | 40   | Data1 (MISO)   | SD_D0       |
| `SPI_CS0`   | 18   | Display IC 0   | —           |
| `SPI_CS1`   | 17   | Display IC 1   | —           |
| `SW_4`      | 21   | —              | SD_CS       |

Both devices cannot drive the bus simultaneously. Below are four approaches to overcome this, ranked from most practical to most involved.

---

## Key Insight: The Display Doesn't Actually Use Quad-SPI

Despite the README saying "QSPI", the actual SPI bus in `comm.c` is configured with only **two data lines** (Data0/Data1) — `quadwp_io_num` and `quadhd_io_num` are both set to `-1`. This means:

- **GPIO 38** (`SPI_Data3`) and **GPIO 39** (`SPI_Data2`) are **defined but never used** by the SPI peripheral
- The bus operates in **standard SPI mode** (or dual-SPI at most), identical to what the SD card needs
- No SPI mode switching is required — both devices speak the same protocol

This makes bus sharing much simpler than it would be with true QSPI.

---

## Approach 1: Shared SPI Bus with ESP-IDF Device Multiplexing (Recommended)

**Complexity: Low | Hardware Changes: None | Risk: Low**

### How It Works

ESP-IDF's SPI driver natively supports **multiple devices on the same bus**. Each device gets its own `spi_device_handle_t` with its own CS pin and clock configuration. The driver guarantees that only one device's CS is asserted at a time.

### Implementation Steps

1. **Register the SD card as a second SPI device** on the existing `SPI3_HOST` bus:
   - CS pin = GPIO 21
   - Clock speed = 10–20 MHz (SD cards are flexible)
   - Use `sdspi_host_init_device()` from ESP-IDF's SDMMC/SPI layer, or Arduino's `SD.begin(21)`

2. **Add a bus mutex / access guard** — a simple flag or semaphore that prevents display and SD card operations from overlapping:
   - Before SD access: assert display CS0 and CS1 HIGH (they already should be, but be explicit)
   - Before display access: assert SD_CS HIGH
   - ESP-IDF handles this automatically per-transaction, but higher-level guards prevent interleaving mid-operation

3. **Sequence operations in the boot flow:**
   ```
   Boot → Read image from SD card → Release SD bus
        → Initialize display → Transfer framebuffer → Refresh (~20s)
        → (Optionally access SD again while display refreshes)
        → Deep sleep
   ```

4. **Leverage the e-ink refresh window** — during the ~20-second e-ink refresh, the display's BUSY pin goes LOW and **no SPI traffic occurs**. The SD card can be accessed freely during this window.

### Pros
- No hardware changes at all — works with the existing board wiring
- ESP-IDF is designed for exactly this pattern
- E-ink display only needs the bus for brief bursts (init + data transfer), leaving long windows for SD access
- Simplest code changes

### Cons
- Cannot read SD card and write to display simultaneously (not needed for this use case)
- Must be disciplined about CS management in firmware
- If SD card is slow or encounters errors, display operations are blocked until SD access completes

### Estimated Code Changes
- `comm.c` / `comm.h`: Add SD card device registration alongside existing display device
- New `SDCardManager.cpp/.h`: SD card init, read file, release bus
- `main.cpp`: Updated boot flow to read from SD before display operations
- `pindefine.h`: Add `#define SD_CS 21` for clarity

---

## Approach 2: SPI Bus Teardown and Rebuild Between Devices

**Complexity: Medium | Hardware Changes: None | Risk: Medium**

### How It Works

Instead of sharing the bus with multiple registered devices, completely **tear down** the SPI bus after display operations and **re-initialize** it for the SD card (and vice versa). This gives each device a completely clean bus state.

### Implementation Steps

1. **Phase 1 — SD Card Access:**
   - Initialize `SPI3_HOST` in standard SPI mode for SD card only (CS = GPIO 21)
   - Read the image file from SD into PSRAM
   - Unmount the SD card filesystem
   - Call `spi_bus_remove_device()` then `spi_bus_free()` to fully release the bus

2. **Phase 2 — Display Access:**
   - Re-initialize `SPI3_HOST` with the display configuration (as currently done in `comm.c`)
   - Register display device with CS0/CS1
   - Transfer framebuffer and trigger refresh
   - Optionally tear down again if SD access is needed post-refresh

### Pros
- Zero chance of bus contention — only one device exists on the bus at any time
- Clean state transitions — no risk of lingering device configuration conflicts
- No shared-bus mutex needed

### Cons
- SPI bus init/teardown takes time and is error-prone
- More complex lifecycle management
- Can't access SD card during display refresh (bus is configured for display)
- More code to maintain

### Estimated Code Changes
- `comm.c` / `comm.h`: Add `deinitSpi()` function, add SD card init function
- `main.cpp`: Orchestrate the two-phase bus lifecycle
- New `SDCardManager.cpp/.h`: SD init/deinit with bus ownership

---

## Approach 3: Use a Second Hardware SPI Bus (SPI2_HOST) with GPIO Rerouting

**Complexity: High | Hardware Changes: Required | Risk: Medium**

### How It Works

The ESP32-S3 has **two usable SPI peripherals**: `SPI2_HOST` and `SPI3_HOST`. If the SD card's physical traces could be rerouted (or an external SD card breakout wired) to different GPIO pins, the SD card could run on `SPI2_HOST` independently.

### Implementation Steps

1. **Hardware modification**: Wire an external SD card breakout board to unused GPIO pins:
   - SD_CLK → e.g., GPIO 12
   - SD_MOSI → e.g., GPIO 11
   - SD_MISO → e.g., GPIO 10
   - SD_CS → e.g., GPIO 14

   (These are potentially available — not used by display, busy, reset, or load switch)

2. **Initialize SPI2_HOST** for the SD card on these alternate pins
3. **Keep SPI3_HOST** exclusively for the display (no changes to existing code)

### Pros
- Complete electrical isolation — both devices can operate truly simultaneously
- No CS management needed between devices
- Existing display code remains completely untouched
- Can stream from SD card while display refreshes

### Cons
- **Requires soldering / hardware modification** — the on-board SD slot is hardwired to the display SPI pins
- Need to verify which GPIO pins are actually broken out on the board's headers/pads
- External SD card breakout board needed
- More complex wiring

### GPIO Availability (ESP32-S3, not used by current firmware)

| GPIO | Status | Notes |
|------|--------|-------|
| 1    | Likely free | Check board schematic |
| 2    | Likely free | Check board schematic |
| 3    | Likely free | Check board schematic |
| 4    | Likely free | Check board schematic |
| 5    | Likely free | Check board schematic |
| 8    | Likely free | Check board schematic |
| 10   | Likely free | Check board schematic |
| 11   | Likely free | Check board schematic |
| 12   | Likely free | Check board schematic |
| 14   | Likely free | Check board schematic |
| 15   | Likely free | Check board schematic |
| 16   | Likely free | Check board schematic |

**Note:** Without the board schematic, we can't confirm which GPIOs are actually broken out to pads/headers. The ESP32-133C02 is a Good-Display board and some pins may be used for internal routing.

---

## Approach 4: SDMMC Peripheral (1-bit SD Mode) via GPIO Matrix

**Complexity: High | Hardware Changes: Possibly Required | Risk: High**

### How It Works

The ESP32-S3 has a dedicated **SDMMC peripheral** separate from SPI. In 1-bit SD mode, it only needs 3 signals (CMD, CLK, D0). If the ESP32-S3's GPIO matrix can route the SDMMC peripheral to GPIO 41 (CMD), 9 (CLK), and 40 (D0), this could work without hardware changes.

### Key Question

The ESP32-S3's SDMMC peripheral typically uses fixed pins (GPIO 34-39 range). Whether the GPIO matrix allows remapping to GPIO 9/40/41 needs verification against the ESP32-S3 TRM.

### Pros
- Completely separate peripheral — no SPI bus sharing
- Hardware-accelerated SD protocol (potentially faster)
- If it works with existing pins, no hardware changes needed

### Cons
- ESP32-S3 SDMMC may not support arbitrary GPIO remapping (unlike SPI which uses the GPIO matrix freely)
- GPIO 9 and 40/41 may conflict with the SPI peripheral even if SDMMC is on a different peripheral, since they're electrically shared
- High risk of incompatibility
- Less community support / documentation for this configuration

### Verdict

This approach is **unlikely to work** without hardware changes because:
1. The electrical signals would still conflict with the display's SPI peripheral
2. SDMMC and SPI use different signaling protocols — the display would see garbage on its data lines when SDMMC is active

---

## Recommendation

**Approach 1 (Shared SPI Bus with ESP-IDF Device Multiplexing)** is the clear winner:

1. **No hardware changes** — works with the existing board as-is
2. **Minimal code changes** — ESP-IDF's SPI driver is designed for multi-device buses
3. **Natural fit for e-ink** — the display only needs the bus briefly, leaving long idle windows for SD access
4. **Low risk** — well-documented pattern in ESP-IDF with many community examples
5. **The display doesn't use true QSPI** — so there's no mode-switching complexity

### Proposed Boot Flow with SD Card

```
Power On
  │
  ├── Initialize SPI3_HOST bus (once)
  ├── Register display device (CS0=18, CS1=17)
  ├── Register SD card device (CS=21)
  │
  ├── Mount SD card filesystem
  ├── Read image file from SD → PSRAM buffer
  ├── Unmount SD card (optional, keeps CS high)
  │
  ├── Initialize EPD (send init commands via CS0/CS1)
  ├── Transfer framebuffer to display via QSPI
  ├── Trigger e-ink refresh (~20 seconds)
  │
  ├── (Optional) Access SD card again during refresh
  │
  └── Deep sleep
```

### Fallback

If Approach 1 encounters unexpected issues (e.g., the SD card library fights with the existing SPI device config), **Approach 2** (teardown/rebuild) is a reliable fallback that still requires zero hardware changes.
