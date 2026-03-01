#include "SDCardManager.h"

#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

extern "C" {
#include "pindefine.h"
}

// The display driver (comm.c) uses SPI3_HOST via ESP-IDF directly.
// The Arduino SPI class defaults to FSPI (SPI2_HOST) on ESP32-S3,
// so we explicitly create an instance on SPI3_HOST (HSPI) using the
// same pins the display will use later.
static SPIClass sdSPI(HSPI);

// LittleFS destination — matches the filename that ImageScreen::loadFromLittleFS() looks for.
static const char *LFS_IMAGE_NAMES[] = {"/local_image.bmp", "/local_image.jpg",
                                         "/local_image.jpeg", "/local_image.png"};

// Files to search for on the SD card (tried in order).
static const char *SD_IMAGE_NAMES[] = {
    "/image.bmp",  "/image.jpg",  "/image.jpeg",  "/image.png",
    "/IMAGE.BMP",  "/IMAGE.JPG",  "/IMAGE.JPEG",  "/IMAGE.PNG",
    "/photo.bmp",  "/photo.jpg",  "/photo.jpeg",  "/photo.png",
    "/display.bmp", "/display.jpg", "/display.jpeg", "/display.png",
};
static const size_t SD_IMAGE_COUNT =
    sizeof(SD_IMAGE_NAMES) / sizeof(SD_IMAGE_NAMES[0]);

// Map an SD card source extension to the LittleFS destination filename.
static const char *lfsDestForExtension(const char *sdPath) {
  String path(sdPath);
  path.toLowerCase();
  if (path.endsWith(".bmp"))
    return "/local_image.bmp";
  if (path.endsWith(".jpg"))
    return "/local_image.jpg";
  if (path.endsWith(".jpeg"))
    return "/local_image.jpeg";
  if (path.endsWith(".png"))
    return "/local_image.png";
  return "/local_image.bin"; // fallback
}

// Remove any existing local_image.* files from LittleFS so we don't
// leave stale images of a different format around.
static void removeOldLocalImages() {
  for (const char *name : LFS_IMAGE_NAMES) {
    if (LittleFS.exists(name)) {
      LittleFS.remove(name);
    }
  }
}

static bool copyFile(fs::FS &srcFS, const char *srcPath, fs::FS &dstFS,
                     const char *dstPath) {
  File src = srcFS.open(srcPath, FILE_READ);
  if (!src) {
    printf("SD: failed to open %s for reading\r\n", srcPath);
    return false;
  }

  size_t fileSize = src.size();
  printf("SD: copying %s (%u bytes) -> LittleFS %s\r\n", srcPath,
         (unsigned)fileSize, dstPath);

  File dst = dstFS.open(dstPath, FILE_WRITE);
  if (!dst) {
    printf("SD: failed to open LittleFS %s for writing\r\n", dstPath);
    src.close();
    return false;
  }

  // Copy in 4 KB chunks to keep stack usage low
  uint8_t buf[4096];
  size_t totalWritten = 0;
  while (totalWritten < fileSize) {
    size_t toRead =
        ((fileSize - totalWritten) < sizeof(buf)) ? (fileSize - totalWritten) : sizeof(buf);
    size_t bytesRead = src.read(buf, toRead);
    if (bytesRead == 0)
      break;
    size_t bytesWritten = dst.write(buf, bytesRead);
    if (bytesWritten != bytesRead) {
      printf("SD: write error (wrote %u of %u)\r\n", (unsigned)bytesWritten,
             (unsigned)bytesRead);
      src.close();
      dst.close();
      return false;
    }
    totalWritten += bytesWritten;
  }

  src.close();
  dst.close();

  printf("SD: copy complete (%u bytes written)\r\n", (unsigned)totalWritten);
  return totalWritten == fileSize;
}

bool copyImageFromSDToLittleFS() {
  printf("SD: Attempting to read image from SD card...\r\n");

  // --- 1. Ensure display CS pins are outputs driven HIGH (deselected) ---
  // This prevents the display from seeing garbage while we talk to the SD card.
  pinMode(SPI_CS0, OUTPUT);
  digitalWrite(SPI_CS0, HIGH);
  pinMode(SPI_CS1, OUTPUT);
  digitalWrite(SPI_CS1, HIGH);

  // --- 2. Initialise SPI bus and mount SD card ---
  sdSPI.begin(SPI_CLK, SPI_Data1, SPI_Data0, SD_CS); // SCK, MISO, MOSI, SS
  if (!SD.begin(SD_CS, sdSPI, 4000000)) { // 4 MHz — safe for init
    printf("SD: card mount failed (no card inserted?)\r\n");
    SD.end();
    sdSPI.end();
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    printf("SD: no card detected\r\n");
    SD.end();
    sdSPI.end();
    return false;
  }
  printf("SD: card detected (type=%u, size=%llu MB)\r\n", cardType,
         SD.cardSize() / (1024 * 1024));

  // --- 3. Find the first matching image file ---
  const char *foundPath = nullptr;
  for (size_t i = 0; i < SD_IMAGE_COUNT; i++) {
    if (SD.exists(SD_IMAGE_NAMES[i])) {
      foundPath = SD_IMAGE_NAMES[i];
      break;
    }
  }

  if (!foundPath) {
    printf("SD: no image file found on card\r\n");
    SD.end();
    sdSPI.end();
    return false;
  }

  // --- 4. Mount LittleFS and copy the image ---
  if (!LittleFS.begin(true)) {
    printf("SD: failed to mount LittleFS\r\n");
    SD.end();
    sdSPI.end();
    return false;
  }

  removeOldLocalImages();
  const char *dstPath = lfsDestForExtension(foundPath);
  bool ok = copyFile(SD, foundPath, LittleFS, dstPath);

  LittleFS.end();

  // --- 5. Tear down SD card and free the SPI bus completely ---
  SD.end();
  sdSPI.end();

  if (ok) {
    printf("SD: image copied successfully. Display will load from LittleFS.\r\n");
  }
  return ok;
}
