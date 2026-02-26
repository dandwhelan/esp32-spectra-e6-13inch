#include "WiFiConnection.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "boards.h"

WiFiConnection::WiFiConnection(const char *ssid, const char *password)
    : _ssid(ssid), _password(password), connected(false) {}

void WiFiConnection::connect() {
  Serial.printf("Connecting to WiFi: %s\n", _ssid);

  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    Serial.println("WiFi already connected, skipping reconnection");
    connected = true;
    Serial.printf("Current IP: %s\n", WiFi.localIP().toString().c_str());
    return;
  }

  WiFi.persistent(false);
  WiFi.setSleep(false);

  const int maxConnectAttempts = 40;  // 20s per connection attempt
  const int maxConnectionCycles = 3;

  connected = false;

  for (int cycle = 1; cycle <= maxConnectionCycles && !connected; cycle++) {
    Serial.printf("WiFi connect cycle %d/%d\n", cycle, maxConnectionCycles);

    // Hard reset WiFi stack state between AP<->STA transitions.
    WiFi.disconnect(true, true);
    delay(500);
    WiFi.mode(WIFI_MODE_NULL);
    delay(200);

    WiFi.mode(WIFI_STA);
    delay(150);

    // Force DHCP mode.
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);

    WiFi.begin(_ssid, _password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < maxConnectAttempts) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println();

    wl_status_t status = WiFi.status();
    Serial.printf("WiFi status after association wait: %d\n", status);

    if (status != WL_CONNECTED) {
      continue;
    }

    Serial.println("Associated to AP, waiting for DHCP lease...");
    const uint32_t dhcpStart = millis();
    const uint32_t dhcpTimeoutMs = 20000;

    while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && (millis() - dhcpStart) < dhcpTimeoutMs) {
      delay(250);
      Serial.print("#");
    }
    Serial.println();

    IPAddress localIP = WiFi.localIP();
    if (localIP == IPAddress(0, 0, 0, 0)) {
      Serial.println("DHCP lease not acquired in this cycle, retrying...");
      continue;
    }

    connected = true;
    Serial.printf("WiFi connected with DHCP IP: %s\n", localIP.toString().c_str());
    Serial.printf("Subnet: %s, Gateway: %s, DNS: %s\n", WiFi.subnetMask().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
  }

  if (!connected) {
    Serial.println("Failed to obtain WiFi connection with DHCP after retries");
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_MODE_NULL);
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

bool WiFiConnection::isConnected() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

void WiFiConnection::checkConnection() {
  bool currentlyConnected = (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0));

  if (!currentlyConnected && connected) {
    Serial.println("WiFi connection lost");
    connected = false;
    reconnect();
  } else if (currentlyConnected && !connected) {
    Serial.println("WiFi reconnected");
    connected = true;
  }
}
