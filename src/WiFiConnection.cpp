#include "WiFiConnection.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>


#include "boards.h"

WiFiConnection::WiFiConnection(const char *ssid, const char *password)
    : _ssid(ssid), _password(password), connected(false) {}

void WiFiConnection::connect() {
  Serial.printf("Connecting to WiFi: %s\n", _ssid);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi already connected, skipping reconnection");
    connected = true;
    return;
  }

  // Completely wipe any previous states (fixes AP-mode interference)
  WiFi.disconnect(true, true);
  delay(500);

  // Explicitly set to Station mode ONLY
  WiFi.mode(WIFI_STA);
  delay(100);

  // Begin standard connection
  WiFi.begin(_ssid, _password);

  int attempts = 0;
  // Give it slightly more time to negotiate DHCP (15 seconds total)
  const int maxAttempts = 30;
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected to WiFi");
    connected = true;
  } else {
    Serial.println("\nFailed to connect to WiFi");
    connected = false;
  }
}

void WiFiConnection::reconnect() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Attempting to reconnect to WiFi...");
    WiFi.disconnect();
    delay(1000);
    connect();
  }
}

bool WiFiConnection::isConnected() { return WiFi.status() == WL_CONNECTED; }

void WiFiConnection::checkConnection() {
  if (WiFi.status() != WL_CONNECTED && connected) {
    Serial.println("WiFi connection lost");
    connected = false;
    reconnect();
  } else if (WiFi.status() == WL_CONNECTED && !connected) {
    Serial.println("WiFi reconnected");
    connected = true;
  }
}
