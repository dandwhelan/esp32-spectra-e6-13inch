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
ImageScreen::ditherImage(uint8_t *rgbBuffer, uint32_t width, uint32_t height) {
  // dithering happens in RGB space with errors carried over
  // we use a signed int buffer for RGB to handle error diffusion
  size_t pixelCount = width * height;
  int16_t *rBuf = (int16_t *)ps_malloc(pixelCount * sizeof(int16_t));
  int16_t *gBuf = (int16_t *)ps_malloc(pixelCount * sizeof(int16_t));
  int16_t *bBuf = (int16_t *)ps_malloc(pixelCount * sizeof(int16_t));

  if (!rBuf || !gBuf || !bBuf) {
    Serial.println("Failed to allocate PSRAM for dithering buffers");
    if (rBuf)
      free(rBuf);
    if (gBuf)
      free(gBuf);
    if (bBuf)
      free(bBuf);
    return nullptr;
  }

  // Copy data to signed buffers
  for (size_t i = 0; i < pixelCount; i++) {
    rBuf[i] = rgbBuffer[i * 3];
    gBuf[i] = rgbBuffer[i * 3 + 1];
    bBuf[i] = rgbBuffer[i * 3 + 2];
  }

  uint8_t *pixelBuffer = (uint8_t *)ps_malloc(pixelCount);
  if (!pixelBuffer) {
    free(rBuf);
    free(gBuf);
    free(bBuf);
    return nullptr;
  }

  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      size_t idx = y * width + x;
      int r = rBuf[idx];
      int g = gBuf[idx];
      int b = bBuf[idx];

      // Find nearest color
      uint8_t colorIdx = findNearestColor(r, g, b);
      pixelBuffer[idx] = colorIdx;

      // Error calculation
      int errR = r - Spectra6Palette[colorIdx].r;
      int errG = g - Spectra6Palette[colorIdx].g;
      int errB = b - Spectra6Palette[colorIdx].b;

      // Floyd-Steinberg diffusion
      auto diffuse = [&](int dx, int dy, int weight) {
        if (x + dx >= 0 && x + dx < width && y + dy < height) {
          size_t targetIdx = (y + dy) * width + (x + dx);
          rBuf[targetIdx] += (errR * weight) / 16;
          gBuf[targetIdx] += (errG * weight) / 16;
          bBuf[targetIdx] += (errB * weight) / 16;
        }
      };

      diffuse(1, 0, 7);
      diffuse(-1, 1, 3);
      diffuse(0, 1, 5);
      diffuse(1, 1, 1);
    }
  }

  free(rBuf);
  free(gBuf);
  free(bBuf);

  // Now create the bitmaps (refactoring logic from processImageData)
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
    free(pixelBuffer);
    return nullptr;
  }

  memset(bitmaps->blackBitmap, 0, bitmapSize);
  memset(bitmaps->yellowBitmap, 0, bitmapSize);
  memset(bitmaps->redBitmap, 0, bitmapSize);
  memset(bitmaps->blueBitmap, 0, bitmapSize);
  memset(bitmaps->greenBitmap, 0, bitmapSize);

  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      uint8_t colorIndex = pixelBuffer[y * width + x];
      int flippedY = (height - 1) - y;
      int byteIndex = flippedY * bitmapWidthBytes + x / 8;
      uint8_t bitMask = 1 << (7 - (x % 8));

      switch (colorIndex) {
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
    }
  }

  free(pixelBuffer);
  return bitmaps;
}

static uint8_t *jpgRgbBuffer = nullptr;
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
        uint16_t pixel = bitmap[j * w + i];
        // RGB565 to RGB888
        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
        uint8_t b = (pixel & 0x1F) << 3;
        size_t idx = (curY * 1200 + curX) * 3;
        jpgRgbBuffer[idx] = r;
        jpgRgbBuffer[idx + 1] = g;
        jpgRgbBuffer[idx + 2] = b;
      }
    }
  }
  return true;
}

std::unique_ptr<ColorImageBitmaps> ImageScreen::decodeJPG(uint8_t *data,
                                                          size_t dataSize) {
  Serial.println("Decoding JPEG...");
  jpgRgbBuffer = (uint8_t *)ps_malloc(1200 * 1600 * 3);
  if (!jpgRgbBuffer) {
    Serial.println("Failed to allocate PSRAM for JPEG RGB buffer");
    return nullptr;
  }

  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpgOutput);

  uint16_t w = 0, h = 0;
  TJpgDec.getJpgSize(&w, &h, data, dataSize);
  Serial.printf("JPEG Size: %dx%d\n", w, h);

  if (TJpgDec.drawJpg(0, 0, data, dataSize) != 0) {
    Serial.println("JPEG decode failed");
    free(jpgRgbBuffer);
    return nullptr;
  }

  auto bitmaps = ditherImage(jpgRgbBuffer, 1200, 1600);
  free(jpgRgbBuffer);
  return bitmaps;
}

static uint8_t *pngRgbBuffer = nullptr;

// PNGdec uses pngDrawCallback defined below

// Actually, let's use a more robust pngDraw implementation
static int pngDrawCallback(PNGDRAW *pDraw) {
  uint16_t lineBuffer[1200]; // Temporary buffer for RGB565
  PNG *png = (PNG *)pDraw->pUser;
  png->getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);

  uint8_t *pOut = pngRgbBuffer + (pDraw->y * 1200 * 3);
  for (int i = 0; i < pDraw->iWidth; i++) {
    uint16_t pixel = lineBuffer[i];
    *pOut++ = ((pixel >> 11) & 0x1F) << 3; // R
    *pOut++ = ((pixel >> 5) & 0x3F) << 2;  // G
    *pOut++ = (pixel & 0x1F) << 3;         // B
  }
  return 1;
}

std::unique_ptr<ColorImageBitmaps> ImageScreen::decodePNG(uint8_t *data,
                                                          size_t dataSize) {
  Serial.println("Decoding PNG...");
  pngRgbBuffer = (uint8_t *)ps_malloc(1200 * 1600 * 3);
  if (!pngRgbBuffer) {
    Serial.println("Failed to allocate PSRAM for PNG RGB buffer");
    return nullptr;
  }
  // Initialize buffer to white to handle transparency or small images
  memset(pngRgbBuffer, 255, 1200 * 1600 * 3);

  PNG png;
  int rc = png.openRAM(data, dataSize, pngDrawCallback);
  if (rc != PNG_SUCCESS) {
    Serial.println("PNG open failed");
    free(pngRgbBuffer);
    return nullptr;
  }

  Serial.printf("PNG Size: %dx%d, Type: %d\n", png.getWidth(), png.getHeight(),
                png.getPixelType());

  rc = png.decode((void *)&png, 0); // Pass png instance as user data
  if (rc != PNG_SUCCESS) {
    Serial.println("PNG decode failed");
    free(pngRgbBuffer);
    return nullptr;
  }

  auto bitmaps = ditherImage(pngRgbBuffer, 1200, 1600);
  free(pngRgbBuffer);
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
    uint8_t *rgbBuffer = (uint8_t *)ps_malloc(1200 * 1600 * 3);
    if (!rgbBuffer)
      return nullptr;
    memset(rgbBuffer, 255, 120 * 1600 * 3);

    dataIndex = dataOffset;
    for (int y = imageHeight - 1; y >= 0; y--) {
      uint8_t *pOut = rgbBuffer + (y * imageWidth * 3);
      for (int x = 0; x < imageWidth; x++) {
        // BMP is BGR
        uint8_t b = data[dataIndex++];
        uint8_t g = data[dataIndex++];
        uint8_t r = data[dataIndex++];
        pOut[x * 3] = r;
        pOut[x * 3 + 1] = g;
        pOut[x * 3 + 2] = b;
      }
      // Skip padding
      dataIndex += (rowSize - imageWidth * 3);
    }
    auto bitmaps = ditherImage(rgbBuffer, 1200, 1600);
    free(rgbBuffer);
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
