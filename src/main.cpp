#include <Arduino.h>

#include "ApplicationConfig.h"
#include "ApplicationConfigStorage.h"
#include "ConfigurationScreen.h"
#include "ConfigurationServer.h"
#include "DisplayAdapter.h"
#include "ImageScreen.h"
#include "WiFiConnection.h"
#include "battery.h"
#include <SPI.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

// Global display instance
DisplayType display;
ApplicationConfigStorage configStorage;
std::unique_ptr<ApplicationConfig> appConfig;

void initializeDefaultConfig() {
  std::unique_ptr<ApplicationConfig> storedConfig = configStorage.load();
  if (storedConfig) {
    appConfig = std::move(storedConfig);
    printf("Configuration loaded from persistent storage: \r\n");
    printf("  - WiFi SSID: %s\n", appConfig->wifiSSID);
  } else {
    appConfig.reset(new ApplicationConfig());
    printf("Using default configuration (no stored config found) \r\n");
  }
}

int displayCurrentScreen(bool isConnected) {
  if (!appConfig->hasValidWiFiCredentials()) {
    printf("No WiFi credentials, showing configuration screen... \r\n");
    ConfigurationScreen configScreen(display);
    configScreen.render();
    return configScreen.nextRefreshInSeconds();
  }

  printf("Showing image screen... \r\n");
  ImageScreen imageScreen(display, *appConfig);
  imageScreen.render();
  return imageScreen.nextRefreshInSeconds();
}

void setup() {
  Serial.begin(115200);
  // Wait for Serial monitor to connect
  unsigned long start = millis();
  while (!Serial && (millis() - start < 3000))
    delay(10);

  printf("\r\n\r\n--- ESP32-133C02 STARTING ---\r\n");
  esp_reset_reason_t reason = esp_reset_reason();
  printf("Reset reason: %d \r\n", (int)reason);
  if (reason == ESP_RST_BROWNOUT) {
    printf("WARNING: Brownout reset detected! Check power supply.\r\n");
  }

  printf("ESP32-133C02 E-Ink Spectra 6 (13.3\") starting... \r\n");
  fflush(stdout);

  initializeDefaultConfig();

  // Connect to WiFi
  if (appConfig->hasValidWiFiCredentials()) {
    WiFiConnection wifi(appConfig->wifiSSID, appConfig->wifiPassword);
    printf("Connecting to WiFi: SSID='%s' \r\n", appConfig->wifiSSID);
    wifi.connect();

    int retry = 0;
    while (!wifi.isConnected() && retry < 20) {
      printf(".");
      delay(1000);
      retry++;
    }
    printf("\r\n");

    if (wifi.isConnected()) {
      printf("WiFi Connected! IP: %s \r\n", WiFi.localIP().toString().c_str());
    } else {
      printf("WiFi Connection Failed. \r\n");
    }

    // --- Display Image Phase (FIRST) ---
    printf("Entering displayCurrentScreen()... \r\n");
    displayCurrentScreen(wifi.isConnected());
    printf("Image displayed successfully.\r\n");

    // --- Web Server Phase: run silently for 10 minutes ---
    Configuration serverConfig(appConfig->wifiSSID, appConfig->wifiPassword,
                               appConfig->imageUrl);
    ConfigurationServer server(serverConfig);

    bool useAP = !wifi.isConnected();
    if (useAP) {
      printf("WiFi failed. Starting Access Point mode...\r\n");
    }

    server.run(
        [](const Configuration &config) {
          printf("Configuration received: SSID=%s, URL=%s\r\n",
                 config.ssid.c_str(), config.imageUrl.c_str());

          strncpy(appConfig->wifiSSID, config.ssid.c_str(),
                  sizeof(appConfig->wifiSSID) - 1);
          strncpy(appConfig->wifiPassword, config.password.c_str(),
                  sizeof(appConfig->wifiPassword) - 1);
          strncpy(appConfig->imageUrl, config.imageUrl.c_str(),
                  sizeof(appConfig->imageUrl) - 1);

          if (configStorage.save(*appConfig)) {
            printf("Configuration saved successfully to NVS.\r\n");
          } else {
            printf("ERROR: Failed to save configuration to NVS.\r\n");
          }
        },
        useAP);

    // Run web server for 10 minutes (600,000 ms) without touching the display
    const unsigned long SERVER_TIMEOUT_MS = 10UL * 60UL * 1000UL;
    unsigned long serverStart = millis();
    printf("Web server running for 10 minutes. Upload images now!\r\n");
    printf("Access it at: http://%s\r\n",
           useAP ? "192.168.4.1" : WiFi.localIP().toString().c_str());

    while (millis() - serverStart < SERVER_TIMEOUT_MS) {
      server.handleRequests();

      // Check if a new image was uploaded — refresh display immediately
      if (server.hasNewImage()) {
        server.clearNewImage();
        printf("New image uploaded! Refreshing display...\r\n");
        displayCurrentScreen(wifi.isConnected());
        printf("Display refreshed with new image.\r\n");
      }

      delay(10);
    }

    printf("Web server timeout reached. Entering permanent deep sleep.\r\n");
    printf("Reset device to start web server again.\r\n");

  } else {
    // No WiFi credentials — show image first, then start AP for setup
    printf("No WiFi credentials. Displaying image first...\r\n");
    displayCurrentScreen(false);

    printf("Starting Access Point mode...\r\n");
    Configuration serverConfig("", "", appConfig->imageUrl);
    ConfigurationServer server(serverConfig);
    server.run(
        [](const Configuration &config) {
          printf("Configuration received (AP mode): SSID=%s\r\n",
                 config.ssid.c_str());
          strncpy(appConfig->wifiSSID, config.ssid.c_str(),
                  sizeof(appConfig->wifiSSID) - 1);
          strncpy(appConfig->wifiPassword, config.password.c_str(),
                  sizeof(appConfig->wifiPassword) - 1);
          strncpy(appConfig->imageUrl, config.imageUrl.c_str(),
                  sizeof(appConfig->imageUrl) - 1);

          if (configStorage.save(*appConfig)) {
            printf("Configuration saved successfully from AP mode.\r\n");
          }
        },
        true);

    // Run AP server for 10 minutes
    const unsigned long SERVER_TIMEOUT_MS = 10UL * 60UL * 1000UL;
    unsigned long serverStart = millis();
    printf("AP server running for 10 minutes.\r\n");

    while (millis() - serverStart < SERVER_TIMEOUT_MS) {
      server.handleRequests();

      if (server.hasNewImage()) {
        server.clearNewImage();
        printf("New image uploaded! Refreshing display...\r\n");
        displayCurrentScreen(false);
        printf("Display refreshed with new image.\r\n");
      }

      delay(10);
    }

    printf("AP server timeout. Entering permanent deep sleep.\r\n");
  }

  // Permanent deep sleep — only power reset wakes the device
  printf("Going to deep sleep forever. Reset to wake.\r\n");
  esp_deep_sleep_start();
}

void loop() {}
