#ifndef __DISPLAY_ADAPTER_H__
#define __DISPLAY_ADAPTER_H__

#include <Adafruit_GFX.h>
#include <Arduino.h>


// Good-Display manufacturer driver (C linkage)
#ifdef __cplusplus
extern "C" {
#endif
#include "GDEP133C02.h"
#include "comm.h"
#include "pindefine.h"
#include "status.h"
#ifdef __cplusplus
}
#endif

// 13.3" Spectra 6 native dimensions (physical pixel layout)
#define EPD_NATIVE_WIDTH 1200
#define EPD_NATIVE_HEIGHT 1600

// Color constants matching GxEPD2 naming for drop-in compatibility
// These map to the Good-Display hardware color codes in GDEP133C02.h
#define GxEPD_BLACK BLACK
#define GxEPD_WHITE WHITE
#define GxEPD_YELLOW YELLOW
#define GxEPD_RED RED
#define GxEPD_BLUE BLUE
#define GxEPD_GREEN GREEN

// DisplayAdapter: wraps the Good-Display QSPI driver to present
// a GxEPD2-compatible API so ImageScreen/ConfigurationScreen work unchanged.
class DisplayAdapter : public Adafruit_GFX {
 public:
  DisplayAdapter();

  // --- GxEPD2-compatible interface ---
  void init(uint32_t serial_diag_bitrate = 115200);
  void setRotation(uint8_t r);
  void setFullWindow();
  void fillScreen(uint16_t color);
  void display(bool partial_update_mode = false);
  void hibernate();

  // Adafruit_GFX required override
  void drawPixel(int16_t x, int16_t y, uint16_t color) override;

  // Additional GFX primitives used by the existing codebase
  // (inherited from Adafruit_GFX: fillRect, fillRoundRect, drawBitmap)

 private:
  // PSRAM framebuffer: 4 bits per pixel, 2 pixels packed per byte
  // Total size: 1200 * 1600 / 2 = 960000 bytes
  uint8_t* _frameBuffer;
  bool _initialized;

  void sendFrameBufferToDisplay();
};

// Type alias so existing code using DisplayType compiles unchanged
using DisplayType = DisplayAdapter;

#endif
