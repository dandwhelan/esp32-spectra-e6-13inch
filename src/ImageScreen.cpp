#include "ImageScreen.h"
#include <Arduino.h>

#include <LittleFS.h>
#include <PNGdec.h>
#include <TJpg_Decoder.h>
#include <WiFi.h>

#include "battery.h"
#include <FS.h>

ImageScreen::ImageScreen(DisplayType &display, ApplicationConfig &config)
    : display(display), config(config), smallFont(u8g2_font_helvR12_tr) {
  gfx.begin(display);
}

void ImageScreen::storeImageETag(const String &etag) {
  strncpy(storedImageETag, etag.c_str(), sizeof(storedImageETag) - 1);
  storedImageETag[sizeof(storedImageETag) - 1] = '\0';
  Serial.println("Stored ETag in RTC memory: " + etag);
}

String ImageScreen::getStoredImageETag() { return String(storedImageETag); }

std::unique_ptr<DownloadResult> ImageScreen::download() {
  String storedETag = getStoredImageETag();
  Serial.println("Using stored ETag for request: '" + storedETag + "'");
  auto result = downloader.download(String(config.imageUrl), storedETag);

  if (result->etag.length() > 0) {
    storeImageETag(result->etag);
  }

  return result;
}

// Spectra 6 palette colors (RGB888)
struct RGBColor {
  uint8_t r, g, b;
};

static const RGBColor Spectra6Palette[] = {
    {0, 0, 0},       // 0: Black
    {255, 255, 255}, // 1: White
    {230, 230, 0},   // 2: Yellow (e6e600)
    {204, 0, 0},     // 3: Red (cc0000)
    {0, 51, 204},    // 4: Blue (0033cc)
    {0, 204, 0}      // 5: Green (00cc00)
};

static uint8_t findNearestColor(int r, int g, int b) {
  uint32_t minDistance = 0xFFFFFFFF;
  uint8_t nearestIndex = 1; // Default to white

  for (uint8_t i = 0; i < 6; i++) {
    int dr = r - Spectra6Palette[i].r;
    int dg = g - Spectra6Palette[i].g;
    int db = b - Spectra6Palette[i].b;
    uint32_t distance = dr * dr + dg * dg + db * db;
    if (distance < minDistance) {
      minDistance = distance;
      nearestIndex = i;
    }
  }
  return nearestIndex;
}

std::unique_ptr<ColorImageBitmaps>
ImageScreen::ditherImage(uint16_t *rgb565Buffer, uint32_t width,
                         uint32_t height) {
  int bitmapWidthBytes = (width + 7) / 8;
  size_t bitmapSize = bitmapWidthBytes * height;

  auto bitmaps = std::unique_ptr<ColorImageBitmaps>(new ColorImageBitmaps());
  bitmaps->width = width;
  bitmaps->height = height;
  bitmaps->bitmapSize = bitmapSize;

  bitmaps->blackBitmap = (uint8_t *)ps_malloc(bitmapSize);
  bitmaps->yellowBitmap = (uint8_t *)ps_malloc(bitmapSize);
  bitmaps->redBitmap = (uint8_t *)ps_malloc(bitmapSize);
  bitmaps->blueBitmap = (uint8_t *)ps_malloc(bitmapSize);
  bitmaps->greenBitmap = (uint8_t *)ps_malloc(bitmapSize);

  if (!bitmaps->blackBitmap || !bitmaps->yellowBitmap || !bitmaps->redBitmap ||
      !bitmaps->blueBitmap || !bitmaps->greenBitmap) {
    Serial.println("Failed to allocate PSRAM for output bitmaps");
    return nullptr;
  }

  memset(bitmaps->blackBitmap, 0, bitmapSize);
  memset(bitmaps->yellowBitmap, 0, bitmapSize);
  memset(bitmaps->redBitmap, 0, bitmapSize);
  memset(bitmaps->blueBitmap, 0, bitmapSize);
  memset(bitmaps->greenBitmap, 0, bitmapSize);

  // Allocate just 2 rows for error diffusion to save huge amounts of PSRAM
  int16_t *errR_curr = (int16_t *)calloc(width, sizeof(int16_t));
  int16_t *errG_curr = (int16_t *)calloc(width, sizeof(int16_t));
  int16_t *errB_curr = (int16_t *)calloc(width, sizeof(int16_t));
  int16_t *errR_next = (int16_t *)calloc(width, sizeof(int16_t));
  int16_t *errG_next = (int16_t *)calloc(width, sizeof(int16_t));
  int16_t *errB_next = (int16_t *)calloc(width, sizeof(int16_t));

  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      size_t idx = y * width + x;
      uint16_t p565 = rgb565Buffer[idx];

      int r = ((p565 >> 11) & 0x1F) << 3;
      int g = ((p565 >> 5) & 0x3F) << 2;
      int b = (p565 & 0x1F) << 3;

      r = constrain(r + errR_curr[x], 0, 255);
      g = constrain(g + errG_curr[x], 0, 255);
      b = constrain(b + errB_curr[x], 0, 255);

      uint8_t colorIdx = findNearestColor(r, g, b);

      int flippedY = (height - 1) - y;
      int byteIndex = flippedY * bitmapWidthBytes + x / 8;
      uint8_t bitMask = 1 << (7 - (x % 8));

      switch (colorIdx) {
      case 0:
        bitmaps->blackBitmap[byteIndex] |= bitMask;
        break;
      case 2:
        bitmaps->yellowBitmap[byteIndex] |= bitMask;
        break;
      case 3:
        bitmaps->redBitmap[byteIndex] |= bitMask;
        break;
      case 4:
        bitmaps->blueBitmap[byteIndex] |= bitMask;
        break;
      case 5:
        bitmaps->greenBitmap[byteIndex] |= bitMask;
        break;
      }

      int errR = r - Spectra6Palette[colorIdx].r;
      int errG = g - Spectra6Palette[colorIdx].g;
      int errB = b - Spectra6Palette[colorIdx].b;

      // Floyd-Steinberg diffusion
      if (x + 1 < width) {
        errR_curr[x + 1] += (errR * 7) / 16;
        errG_curr[x + 1] += (errG * 7) / 16;
        errB_curr[x + 1] += (errB * 7) / 16;
      }
      if (y + 1 < height) {
        if (x > 0) {
          errR_next[x - 1] += (errR * 3) / 16;
          errG_next[x - 1] += (errG * 3) / 16;
          errB_next[x - 1] += (errB * 3) / 16;
        }
        errR_next[x] += (errR * 5) / 16;
        errG_next[x] += (errG * 5) / 16;
        errB_next[x] += (errB * 5) / 16;

        if (x + 1 < width) {
          errR_next[x + 1] += (errR * 1) / 16;
          errG_next[x + 1] += (errG * 1) / 16;
          errB_next[x + 1] += (errB * 1) / 16;
        }
      }
    }
    memcpy(errR_curr, errR_next, width * sizeof(int16_t));
    memcpy(errG_curr, errG_next, width * sizeof(int16_t));
    memcpy(errB_curr, errB_next, width * sizeof(int16_t));
    memset(errR_next, 0, width * sizeof(int16_t));
    memset(errG_next, 0, width * sizeof(int16_t));
    memset(errB_next, 0, width * sizeof(int16_t));
  }

  free(errR_curr);
  free(errG_curr);
  free(errB_curr);
  free(errR_next);
  free(errG_next);
  free(errB_next);

  return bitmaps;
}

static uint16_t *jpgRgb565Buffer = nullptr;
static uint32_t jpgWidth = 0;

bool ImageScreen::jpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h,
                            uint16_t *bitmap) {
  if (y >= 1600 || x >= 1200)
    return true;

  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int curX = x + i;
      int curY = y + j;
      if (curX < 1200 && curY < 1600) {
        size_t idx = curY * 1200 + curX;
        jpgRgb565Buffer[idx] = bitmap[j * w + i];
      }
    }
  }
  return true;
}

std::unique_ptr<ColorImageBitmaps> ImageScreen::decodeJPG(uint8_t *data,
                                                          size_t dataSize) {
  Serial.println("Decoding JPEG...");
  jpgRgb565Buffer = (uint16_t *)ps_malloc(1200 * 1600 * 2);
  if (!jpgRgb565Buffer) {
    Serial.println("Failed to allocate PSRAM for JPEG RGB565 buffer");
    return nullptr;
  }
  memset(jpgRgb565Buffer, 0xFFFF, 1200 * 1600 * 2); // White background

  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpgOutput);

  uint16_t w = 0, h = 0;
  TJpgDec.getJpgSize(&w, &h, data, dataSize);
  Serial.printf("JPEG Size: %dx%d\n", w, h);

  if (TJpgDec.drawJpg(0, 0, data, dataSize) != 0) {
    Serial.println("JPEG decode failed");
    free(jpgRgb565Buffer);
    return nullptr;
  }

  auto bitmaps = ditherImage(jpgRgb565Buffer, 1200, 1600);
  free(jpgRgb565Buffer);
  return bitmaps;
}

static uint16_t *pngRgb565Buffer = nullptr;

static int pngDrawCallback(PNGDRAW *pDraw) {
  PNG *png = (PNG *)pDraw->pUser;
  png->getLineAsRGB565(pDraw, &pngRgb565Buffer[pDraw->y * 1200],
                       PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);
  return 1;
}

std::unique_ptr<ColorImageBitmaps> ImageScreen::decodePNG(uint8_t *data,
                                                          size_t dataSize) {
  Serial.println("Decoding PNG...");
  pngRgb565Buffer = (uint16_t *)ps_malloc(1200 * 1600 * 2);
  if (!pngRgb565Buffer) {
    Serial.println("Failed to allocate PSRAM for PNG RGB565 buffer");
    return nullptr;
  }
  memset(pngRgb565Buffer, 0xFFFF, 1200 * 1600 * 2);

  PNG png;
  int rc = png.openRAM(data, dataSize, pngDrawCallback);
  if (rc != PNG_SUCCESS) {
    Serial.println("PNG open failed");
    free(pngRgb565Buffer);
    return nullptr;
  }

  Serial.printf("PNG Size: %dx%d, Type: %d\n", png.getWidth(), png.getHeight(),
                png.getPixelType());

  rc = png.decode((void *)&png, 0);
  if (rc != PNG_SUCCESS) {
    Serial.println("PNG decode failed");
    free(pngRgb565Buffer);
    return nullptr;
  }

  auto bitmaps = ditherImage(pngRgb565Buffer, 1200, 1600);
  free(pngRgb565Buffer);
  return bitmaps;
}

std::unique_ptr<ColorImageBitmaps> ImageScreen::decodeBMP(uint8_t *data,
                                                          size_t dataSize) {
  size_t dataIndex = 0;

  if (dataSize < 54) {
    Serial.printf("Payload too small for BMP header: got %d bytes, expected at "
                  "least 54\n",
                  dataSize);
    return nullptr;
  }

  uint8_t bmpHeader[54];
  memcpy(bmpHeader, data + dataIndex, 54);
  dataIndex += 54;

  if (bmpHeader[0] != 'B' || bmpHeader[1] != 'M') {
    return nullptr;
  }

  uint32_t dataOffset = bmpHeader[10] | (bmpHeader[11] << 8) |
                        (bmpHeader[12] << 16) | (bmpHeader[13] << 24);
  uint32_t imageWidth = bmpHeader[18] | (bmpHeader[19] << 8) |
                        (bmpHeader[20] << 16) | (bmpHeader[21] << 24);
  uint32_t imageHeight = bmpHeader[22] | (bmpHeader[23] << 8) |
                         (bmpHeader[24] << 16) | (bmpHeader[25] << 24);
  uint16_t bitsPerPixel = bmpHeader[28] | (bmpHeader[29] << 8);
  uint32_t compression = bmpHeader[30] | (bmpHeader[31] << 8) |
                         (bmpHeader[32] << 16) | (bmpHeader[33] << 24);

  // If it's a standard 8-bit BMP (like from's dither tool), we handle it
  // specifically
  if (bitsPerPixel == 8 && compression == 0) {
    // (Old logic for pre-dithered BMPs)
    uint32_t paletteSize = 256 * 4;
    dataIndex += paletteSize;
    if (dataOffset > dataIndex)
      dataIndex += (dataOffset - dataIndex);

    uint32_t rowSize = ((imageWidth * bitsPerPixel + 31) / 32) * 4;
    uint8_t *rowBuffer = new uint8_t[rowSize];
    uint8_t *pixelBuffer = (uint8_t *)ps_malloc(imageWidth * imageHeight);

    for (int y = imageHeight - 1; y >= 0; y--) {
      memcpy(rowBuffer, data + dataIndex, rowSize);
      dataIndex += rowSize;
      for (int x = 0; x < imageWidth; x++) {
        pixelBuffer[((imageHeight - 1) - y) * imageWidth + x] = rowBuffer[x];
      }
    }
    delete[] rowBuffer;

    // Convert mapping logic... wait, if it's already dithered to indices 0-5,
    // we can just map them.
    // But for native BMP support (random 24bit BMP), we should decode to
    // RGB888.
  }

  // FALLBACK: For non-8bit BMP or generic BMP, we should ideally decode to
  // RGB888. For now, let's assume if it starts with 'BM' and isn't our special
  // 8bit, we might need a BMP library or just simple 24bit parsing.

  if (bitsPerPixel == 24) {
    Serial.println("Decoding 24-bit BMP...");
    uint32_t rowSize = ((imageWidth * 24 + 31) / 32) * 4;
    uint16_t *rgb565Buffer = (uint16_t *)ps_malloc(1200 * 1600 * 2);
    if (!rgb565Buffer)
      return nullptr;
    memset(rgb565Buffer, 0xFFFF, 1200 * 1600 * 2); // White background

    dataIndex = dataOffset;
    for (int y = imageHeight - 1; y >= 0; y--) {
      uint16_t *pOut = rgb565Buffer + (y * 1200);
      for (int x = 0; x < imageWidth; x++) {
        if (x >= 1200 || y >= 1600) {
          dataIndex += 3;
          continue;
        }
        // BMP is BGR
        uint8_t b = data[dataIndex++];
        uint8_t g = data[dataIndex++];
        uint8_t r = data[dataIndex++];

        // Convert to RGB565
        uint16_t r5 = (r >> 3) & 0x1F;
        uint16_t g6 = (g >> 2) & 0x3F;
        uint16_t b5 = (b >> 3) & 0x1F;
        pOut[x] = (r5 << 11) | (g6 << 5) | b5;
      }
      // Skip padding
      dataIndex += (rowSize - imageWidth * 3);
    }
    auto bitmaps = ditherImage(rgb565Buffer, 1200, 1600);
    free(rgb565Buffer);
    return bitmaps;
  }

  return nullptr; // Unsupported BMP format
}

std::unique_ptr<ColorImageBitmaps>
ImageScreen::processImageData(uint8_t *data, size_t dataSize) {
  if (dataSize < 4)
    return nullptr;

  // Manual format detection
  if (data[0] == 0xFF && data[1] == 0xD8) {
    return decodeJPG(data, dataSize);
  } else if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
             data[3] == 'G') {
    return decodePNG(data, dataSize);
  } else if (data[0] == 'B' && data[1] == 'M') {
    return decodeBMP(data, dataSize);
  }

  Serial.println("Unknown image format");
  return nullptr;
}

void ImageScreen::renderBitmaps(const ColorImageBitmaps &bitmaps) {
  // Calculate position to center the image on display
  int displayWidth = display.width();
  int displayHeight = display.height();

  // For same-size image, just position at origin
  int imageX = 0;
  int imageY = 0;

  // Only center if image is smaller than display
  if ((int)bitmaps.width < displayWidth) {
    imageX = (displayWidth - (int)bitmaps.width) / 2;
  }
  if ((int)bitmaps.height < displayHeight) {
    imageY = (displayHeight - (int)bitmaps.height) / 2;
  }

  // Ensure coordinates are valid
  imageX = max(0, imageX);
  imageY = max(0, imageY);

  // Draw all color bitmaps directly
  display.drawBitmap(imageX, imageY, bitmaps.blackBitmap, bitmaps.width,
                     bitmaps.height, GxEPD_BLACK);
  display.drawBitmap(imageX, imageY, bitmaps.yellowBitmap, bitmaps.width,
                     bitmaps.height, GxEPD_YELLOW);
  display.drawBitmap(imageX, imageY, bitmaps.redBitmap, bitmaps.width,
                     bitmaps.height, GxEPD_RED);
  display.drawBitmap(imageX, imageY, bitmaps.blueBitmap, bitmaps.width,
                     bitmaps.height, GxEPD_BLUE);
  display.drawBitmap(imageX, imageY, bitmaps.greenBitmap, bitmaps.width,
                     bitmaps.height, GxEPD_GREEN);
}

std::unique_ptr<ColorImageBitmaps> ImageScreen::loadFromLittleFS() {
  const char *extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ""};
  String baseName = "/local_image";
  String filename = "";

  for (const char *ext : extensions) {
    if (LittleFS.exists(baseName + ext)) {
      filename = baseName + ext;
      break;
    }
  }

  if (filename == "") {
    printf("No local image found on LittleFS.\r\n");
    return nullptr;
  }

  File file = LittleFS.open(filename, FILE_READ);
  if (!file) {
    printf("Failed to open %s for reading.\r\n", filename.c_str());
    return nullptr;
  }

  size_t fileSize = file.size();
  printf("Found %s (Size: %d bytes). Loading into PSRAM...\r\n",
         filename.c_str(), fileSize);

  uint8_t *fileBuffer = (uint8_t *)ps_malloc(fileSize);
  if (!fileBuffer) {
    printf("Failed to allocate %d bytes in PSRAM for LittleFS image.\r\n",
           fileSize);
    file.close();
    return nullptr;
  }

  size_t bytesRead = file.read(fileBuffer, fileSize);
  file.close();

  if (bytesRead != fileSize) {
    printf("Warning: Read %d bytes, expected %d bytes\r\n", bytesRead,
           fileSize);
  }

  printf("Passing local LittleFS image to processImageData...\r\n");
  auto bitmaps = processImageData(fileBuffer, bytesRead);

  free(fileBuffer);

  return bitmaps;
}

void ImageScreen::render() {
  display.init(115200);
  display.setRotation(ApplicationConfig::DISPLAY_ROTATION);
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);

  LittleFS.begin(true);
  auto bitmaps = loadFromLittleFS();

  if (bitmaps) {
    printf("Rendering local image from LittleFS\r\n");
    renderBitmaps(*bitmaps);
    displayBatteryStatus();
    displayWifiInfo();
    display.display();
    display.hibernate();
    return;
  }

  auto downloadResult = download();

  if (downloadResult->httpCode == HTTP_CODE_NOT_MODIFIED) {
    Serial.println("Image not modified (304), using cached version");
    return;
  }

  if (downloadResult->httpCode != HTTP_CODE_OK) {
    printf("Failed to download image (HTTP %d)\r\n", downloadResult->httpCode);
    return;
  }

  bitmaps = processImageData(downloadResult->data, downloadResult->size);
  if (!bitmaps) {
    printf("Failed to process image data\r\n");
    return;
  }

  renderBitmaps(*bitmaps);
  displayBatteryStatus();
  displayWifiInfo();

  display.display();
  display.hibernate();
}

void ImageScreen::displayBatteryStatus() {
  String batteryStatus = getBatteryStatus();
  gfx.setFontMode(0);
  gfx.setBackgroundColor(GxEPD_WHITE);
  gfx.setForegroundColor(GxEPD_BLACK);
  gfx.setFont(u8g2_font_helvB08_tr);

  int textWidth = gfx.getUTF8Width(batteryStatus.c_str());
  int textHeight = gfx.getFontAscent() - gfx.getFontDescent();
  int fontAscent = gfx.getFontAscent();

  int paddingX = 6;
  int paddingY = 4;
  int rectWidth = textWidth + (2 * paddingX);
  int rectHeight = textHeight + (2 * paddingY);

  int rectX = display.width() - rectWidth - 18;
  int rectY = display.height() - rectHeight - 4;

  // Calculate centered text position within the rectangle
  int batteryX = rectX + (rectWidth - textWidth) / 2;
  int batteryY = rectY + rectHeight / 2 + fontAscent / 2;

  // Draw white rounded rectangle background
  display.fillRoundRect(rectX, rectY, rectWidth, rectHeight, 4, GxEPD_WHITE);

  gfx.setCursor(batteryX, batteryY);
  gfx.print(batteryStatus);
}

void ImageScreen::displayWifiInfo() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  String wifiInfo =
      String(WiFi.SSID()) + " (" + WiFi.localIP().toString() + ")";

  gfx.setFontMode(0);
  gfx.setBackgroundColor(GxEPD_WHITE);
  gfx.setForegroundColor(GxEPD_BLACK);
  gfx.setFont(u8g2_font_helvB08_tr);

  int textWidth = gfx.getUTF8Width(wifiInfo.c_str());
  int textHeight = gfx.getFontAscent() - gfx.getFontDescent();
  int fontAscent = gfx.getFontAscent();

  int paddingX = 6;
  int paddingY = 4;
  int rectWidth = textWidth + (2 * paddingX);
  int rectHeight = textHeight + (2 * paddingY);

  // Position at the bottom left
  int rectX = 4;
  int rectY = display.height() - rectHeight - 4;

  int infoX = rectX + paddingX;
  int infoY = rectY + rectHeight / 2 + fontAscent / 2;

  // Draw white rounded rectangle background
  display.fillRoundRect(rectX, rectY, rectWidth, rectHeight, 4, GxEPD_WHITE);

  gfx.setCursor(infoX, infoY);
  gfx.print(wifiInfo);
}

int ImageScreen::nextRefreshInSeconds() { return 1800; }
