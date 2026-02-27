#include <Arduino.h>

#include "ApplicationConfig.h"
#include "ApplicationConfigStorage.h"
#include "ConfigurationScreen.h"
#include "ConfigurationServer.h"
#include "DisplayAdapter.h"
#include "ImageScreen.h"
#include "WiFiConnection.h"
#include "battery.h"

// Global display instance
DisplayType display;
ApplicationConfigStorage configStorage;
std::unique_ptr<ApplicationConfig> appConfig;

void goToSleep(int sleepTimeInSeconds) {
  printf("Timer-only wakeup (button wakeup disabled for testing) \r\n");

  uint64_t sleepTimeMicros = (uint64_t)sleepTimeInSeconds * 1000000ULL;
  esp_sleep_enable_timer_wakeup(sleepTimeMicros);
  esp_deep_sleep_start();
}

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

  if (appConfig->hasValidWiFiCredentials()) {
    WiFiConnection wifi(appConfig->wifiSSID, appConfig->wifiPassword);
    printf("Connecting to WiFi: SSID='%s' \r\n", appConfig->wifiSSID);
    wifi.connect();

    // Progress indicator
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

    printf("Entering displayCurrentScreen()... \r\n");
    int refreshSeconds = displayCurrentScreen(true);
    printf("setup() finished, going to sleep. \r\n");
    goToSleep(refreshSeconds);
  } else {
    printf("Entering displayCurrentScreen() (No WiFi)... \r\n");
    int refreshSeconds = displayCurrentScreen(false);
    printf("setup() finished (AP mode), going to sleep. \r\n");
    goToSleep(refreshSeconds);
  }
}

void loop() {}
