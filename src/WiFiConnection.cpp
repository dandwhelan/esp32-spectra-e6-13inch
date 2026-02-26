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
    Serial.printf("Current IP: %s\n", WiFi.localIP().toString().c_str());
    return;
  }

  // Completely wipe any previous states (fixes AP-mode interference)
  WiFi.disconnect(true, true);
  delay(500);

  // Explicitly set to Station mode ONLY
  WiFi.mode(WIFI_STA);
  delay(100);

  // Force DHCP client for station mode before begin()
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);

  // Begin standard connection
  WiFi.begin(_ssid, _password);

  int attempts = 0;
  // Give it more time to negotiate association + DHCP lease
  const int maxAttempts = 60;
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected to WiFi, waiting for DHCP lease...");

    const uint32_t dhcpStart = millis();
    const uint32_t dhcpTimeoutMs = 15000;
    while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && (millis() - dhcpStart) < dhcpTimeoutMs) {
      delay(250);
      Serial.print("#");
    }
    Serial.println();

    IPAddress localIP = WiFi.localIP();
    if (localIP != IPAddress(0, 0, 0, 0)) {
      connected = true;
      Serial.printf("WiFi connected with DHCP IP: %s\n", localIP.toString().c_str());
      Serial.printf("Gateway: %s, DNS: %s\n", WiFi.gatewayIP().toString().c_str(),
                    WiFi.dnsIP().toString().c_str());
    } else {
      connected = false;
      Serial.println("Connected to AP but DHCP lease was not obtained in time");
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_STA);
    }
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
