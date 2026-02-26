#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <time.h>

#include <memory>

#include "ApplicationConfig.h"
#include "ApplicationConfigStorage.h"
#include "ConfigurationScreen.h"
#include "ConfigurationServer.h"
#include "DisplayAdapter.h"
#include "ImageScreen.h"
#include "WiFiConnection.h"
#include "battery.h"
#include "esp32-hal.h"

std::unique_ptr<ApplicationConfig> appConfig;
ApplicationConfigStorage configStorage;

// Display instance — our adapter handles all QSPI communication internally
DisplayType display;

void goToSleep(uint64_t sleepTimeInSeconds);
int displayCurrentScreen(bool wifiConnected);
bool isButtonWakeup();
void updateConfiguration(const Configuration &config);
void initializeDefaultConfig();

bool isButtonWakeup() {
  esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();
  Serial.printf("Wakeup cause: %d (EXT0=%d, TIMER=%d)\n", wakeupReason,
                ESP_SLEEP_WAKEUP_EXT0, ESP_SLEEP_WAKEUP_TIMER);
  return (wakeupReason == ESP_SLEEP_WAKEUP_EXT0);
}

int displayCurrentScreen(bool wifiConnected) {
  if (!appConfig->hasValidWiFiCredentials() || !wifiConnected) {
    if (!appConfig->hasValidWiFiCredentials()) {
      Serial.println("No valid WiFi credentials, showing configuration screen");
    } else {
      Serial.println("Failed to connect to WiFi, showing configuration screen");
    }

    ConfigurationScreen configurationScreen(display);
    configurationScreen.render();

    Configuration currentConfig = Configuration(
        appConfig->wifiSSID, appConfig->wifiPassword, appConfig->imageUrl);
    ConfigurationServer configurationServer(currentConfig);

    configurationServer.run(updateConfiguration);

    while (configurationServer.isRunning()) {
      configurationServer.handleRequests();
      delay(10);
    }

    configurationServer.stop();
    return configurationScreen.nextRefreshInSeconds();
  } else {
    ImageScreen imageScreen(display, *appConfig);
    imageScreen.render();
    return imageScreen.nextRefreshInSeconds();
  }
}

void updateConfiguration(const Configuration &config) {
  if (config.ssid.length() >= sizeof(appConfig->wifiSSID)) {
    Serial.println("Error: SSID too long, maximum length is " +
                   String(sizeof(appConfig->wifiSSID) - 1));
    return;
  }

  if (config.password.length() >= sizeof(appConfig->wifiPassword)) {
    Serial.println("Error: Password too long, maximum length is " +
                   String(sizeof(appConfig->wifiPassword) - 1));
    return;
  }

  if (config.imageUrl.length() >= sizeof(appConfig->imageUrl)) {
    Serial.println("Error: Image URL too long, maximum length is " +
                   String(sizeof(appConfig->imageUrl) - 1));
    return;
  }

  memset(appConfig->wifiSSID, 0, sizeof(appConfig->wifiSSID));
  memset(appConfig->wifiPassword, 0, sizeof(appConfig->wifiPassword));
  memset(appConfig->imageUrl, 0, sizeof(appConfig->imageUrl));

  strncpy(appConfig->wifiSSID, config.ssid.c_str(),
          sizeof(appConfig->wifiSSID) - 1);
  strncpy(appConfig->wifiPassword, config.password.c_str(),
          sizeof(appConfig->wifiPassword) - 1);
  strncpy(appConfig->imageUrl, config.imageUrl.c_str(),
          sizeof(appConfig->imageUrl) - 1);

  // Save configuration to persistent storage
  bool saved = configStorage.save(*appConfig);
  if (saved) {
    Serial.println("Configuration saved to persistent storage");
  } else {
    Serial.println("Failed to save configuration to persistent storage");
  }

  Serial.println("Configuration updated");
  Serial.println("WiFi SSID: " + String(appConfig->wifiSSID));
  Serial.println("Image URL: " + String(strlen(appConfig->imageUrl) > 0
                                            ? appConfig->imageUrl
                                            : "[NOT SET]"));

  Serial.println("Rebooting device to apply new configuration...");
  delay(1000);
  ESP.restart();
}

void goToSleep(uint64_t sleepTimeInSeconds) {
  Serial.println("Going to deep sleep for " + String(sleepTimeInSeconds) +
                 " seconds");
  Serial.println("Timer-only wakeup (button wakeup disabled for testing)");

  uint64_t sleepTimeMicros = sleepTimeInSeconds * 1000000ULL;
  esp_sleep_enable_timer_wakeup(sleepTimeMicros);
  esp_deep_sleep_start();
}

void initializeDefaultConfig() {
  std::unique_ptr<ApplicationConfig> storedConfig = configStorage.load();
  if (storedConfig) {
    appConfig = std::move(storedConfig);
    Serial.println("Configuration loaded from persistent storage: ");
    Serial.printf("  - WiFi SSID: %s\n", appConfig->wifiSSID);
    Serial.printf("  - Image URL: %s\n", strlen(appConfig->imageUrl) > 0
                                             ? appConfig->imageUrl
                                             : "[NOT SET]");
  } else {
    appConfig.reset(new ApplicationConfig());
    Serial.println("Using default configuration (no stored config found)");
  }
}

void setup() {
  delay(1000);
  // Match platformio.ini monitor_speed to avoid unreadable serial output.
  Serial.begin(460800);

  Serial.println("ESP32-133C02 E-Ink Spectra 6 (13.3\") starting...");

  initializeDefaultConfig();

  // Note: SPI and GPIO are initialized by DisplayAdapter::init() via the
  // manufacturer driver. No need for manual SPI.begin() or boards.h pin setup.

  // Try to connect to WiFi if we have valid credentials
  WiFiConnection wifi(appConfig->wifiSSID, appConfig->wifiPassword);

  if (appConfig->hasValidWiFiCredentials()) {
    Serial.printf("WiFi credentials loaded: SSID='%s', Password length=%d\n",
                  appConfig->wifiSSID, strlen(appConfig->wifiPassword));
    wifi.connect();
  }

  int refreshSeconds = displayCurrentScreen(wifi.isConnected());
  goToSleep(refreshSeconds);
}

void loop() {}
