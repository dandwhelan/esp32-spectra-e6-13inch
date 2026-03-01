#ifndef SD_CARD_MANAGER_H
#define SD_CARD_MANAGER_H

#include <Arduino.h>

// Copies the first image found on the SD card to LittleFS, then fully
// releases the SPI bus so the display driver can re-initialise it later.
//
// Call this BEFORE any display initialisation — the SD card and the
// e-ink display share the same SPI bus (SPI3_HOST) and cannot be used
// at the same time.
//
// Supported image files (searched in order):
//   /image.bmp, /image.jpg, /image.jpeg, /image.png
//
// Returns true if an image was successfully copied to LittleFS.
bool copyImageFromSDToLittleFS();

#endif
