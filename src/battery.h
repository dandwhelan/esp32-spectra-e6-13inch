#pragma once

#include <Arduino.h>

// #include "boards.h" // Removed: caused pin conflicts with GDEP133C02

#define BATTERY_MAX_VOLTAGE 4.2
#define BATTERY_MIN_VOLTAGE 3.3
#define VOLTAGE_DIVIDER_RATIO 2.0

// Note: BATTERY_PIN is device-specific. Defining as -1 to disable for now.
#define BATTERY_PIN -1

String getBatteryStatus();