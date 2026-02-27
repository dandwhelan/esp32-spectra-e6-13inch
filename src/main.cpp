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
#include <esp_sleep.h>

// Global display instance
DisplayType display;
ApplicationConfigStorage configStorage;
std::unique_ptr<ApplicationConfig> appConfig;

void goToSleep(int sleepTimeInSeconds) {
  printf("Deep sleep for %d seconds. Press Switch 2 to wake for uploads.\r\n",
         sleepTimeInSeconds);

  uint64_t sleepTimeMicros = (uint64_t)sleepTimeInSeconds * 1000000ULL;
  esp_sleep_enable_timer_wakeup(sleepTimeMicros);

  // Enable Switch 2 (GPIO 13) as wakeup source
  esp_sleep_enable_ext0_wakeup((gpio_num_t)13, 0); // 0 = Wake on LOW

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

  initializeDefaultConfig();

  // Initial check based on sleep wake cause
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool webServerMode = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);

  // Fallback check for power-on or reset buttons (if held)
  pinMode(13, INPUT_PULLUP);
  delay(50);

  // Only check the hardware button if it's NOT a software restart.
  // Software restarts (ESP_RST_SW) occur after an image upload or clear.
  if (reason != ESP_RST_SW) {
    if (digitalRead(13) == LOW) {
      webServerMode = true;
    }
  }

  if (webServerMode) {
    printf("Wakeup caused by GPIO (Switch 2) or button held. Web Server Mode "
           "enabled.\r\n");
  } else {
    printf("Wakeup caused by Timer/Power-on. Normal operation mode.\r\n");
  }

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

    if (webServerMode) {
      if (!wifi.isConnected()) {
        printf(
            "WiFi failed to connect. Falling back to Access Point mode...\r\n");
      }

      Configuration serverConfig(appConfig->wifiSSID, appConfig->wifiPassword,
                                 appConfig->imageUrl);
      ConfigurationServer server(serverConfig);

      bool useAP = !wifi.isConnected();
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

      display.init(115200);
      display.setRotation(ApplicationConfig::DISPLAY_ROTATION);
      display.setFullWindow();
      display.fillScreen(GxEPD_WHITE);
      display.setTextColor(GxEPD_BLACK);
      display.setTextSize(3);
      display.setCursor(50, 100);
      display.println("Web Server Running");
      display.setCursor(50, 150);

      if (useAP) {
        display.println("AP: " + server.getWifiAccessPointName());
        display.setCursor(50, 200);
        display.println("IP: 192.168.4.1");
      } else {
        display.println("IP: " + WiFi.localIP().toString());
      }

      display.setTextSize(2);
      display.setCursor(50, 270);
      display.println("Upload images or configure WiFi.");
      display.display();
      display.hibernate();

      printf("Entering infinite server loop. RESET device to resume normal "
             "operation.\r\n");
      while (true) {
        server.handleRequests();
        delay(10);
      }
    }

    printf("Entering displayCurrentScreen()... \r\n");
    int refreshSeconds = displayCurrentScreen(true);
    printf("setup() finished, going to sleep. \r\n");

    // Wait for button release to avoid immediate wakeup loop
    if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
      printf("Waiting for Switch 2 release...\r\n");
      while (digitalRead(13) == LOW) {
        delay(10);
      }
      delay(100); // Debounce
    }

    goToSleep(refreshSeconds);
  } else {
    // No WiFi credentials, or forced Web Server mode
    if (webServerMode) {
      printf("No WiFi credentials. Starting in Access Point mode...\r\n");
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

      display.init(115200);
      display.setRotation(ApplicationConfig::DISPLAY_ROTATION);
      display.setFullWindow();
      display.fillScreen(GxEPD_WHITE);
      display.setTextColor(GxEPD_BLACK);
      display.setTextSize(3);
      display.setCursor(50, 100);
      display.println("Config Mode (AP)");
      display.setCursor(50, 150);
      display.println("SSID: " + server.getWifiAccessPointName());
      display.setCursor(50, 200);
      display.println("IP: 192.168.4.1");
      display.display();
      display.hibernate();

      while (true) {
        server.handleRequests();
        delay(10);
      }
    }

    printf("Entering displayCurrentScreen() (No WiFi)... \r\n");
    int refreshSeconds = displayCurrentScreen(false);
    printf("setup() finished (AP mode), going to sleep. \r\n");
    goToSleep(refreshSeconds);
  }
}

void loop() {}
