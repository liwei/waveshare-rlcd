#include "battery.h"

#include <Arduino.h>

#include "config.h"

static constexpr uint8_t kSamplesPerReading = 8;
static constexpr uint8_t kTrendWindowSamples = 4;

static uint32_t s_lastSampleMs;
static uint16_t s_history[kTrendWindowSamples];
static uint8_t s_historyCount;
static uint8_t s_historyNext;
static uint8_t s_risingWindows;
static uint8_t s_flatWindows;
static BatteryStatus s_status = {0, false};

static uint16_t readBatteryMilliVolts() {
  uint32_t total = 0;
  for (uint8_t i = 0; i < kSamplesPerReading; ++i) {
    total += analogReadMilliVolts(BATTERY_ADC_PIN);
  }
  return (uint16_t)((total * 3U + kSamplesPerReading / 2) / kSamplesPerReading);
}

void Battery_Init() {
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  s_lastSampleMs = millis() - BATTERY_SAMPLE_INTERVAL_MS;
  Battery_Update();
}

bool Battery_Update() {
  const uint32_t now = millis();
  if (now - s_lastSampleMs < BATTERY_SAMPLE_INTERVAL_MS) return false;
  const BatteryStatus previousStatus = s_status;
  s_lastSampleMs = now;

  const uint16_t millivolts = readBatteryMilliVolts();
  uint32_t filteredTotal = millivolts;
  for (uint8_t i = 0; i < s_historyCount; ++i) filteredTotal += s_history[i];
  const uint16_t filtered = (uint16_t)(filteredTotal / (s_historyCount + 1));

  if (s_historyCount == kTrendWindowSamples) {
    const uint16_t oldest = s_history[s_historyNext];
    if (filtered >= oldest + BATTERY_CHARGE_RISE_MV) {
      if (s_risingWindows < BATTERY_CHARGE_SAMPLES) ++s_risingWindows;
      s_flatWindows = 0;
    } else {
      s_risingWindows = 0;
      if (s_flatWindows < BATTERY_CHARGE_SAMPLES) ++s_flatWindows;
    }
    if (s_risingWindows >= BATTERY_CHARGE_SAMPLES) s_status.charging = true;
    if (s_flatWindows >= BATTERY_CHARGE_SAMPLES) s_status.charging = false;
  }

  s_history[s_historyNext] = filtered;
  s_historyNext = (s_historyNext + 1) % kTrendWindowSamples;
  if (s_historyCount < kTrendWindowSamples) ++s_historyCount;
  if (millivolts <= 3000) {
    s_status.percent = 0;
  } else if (millivolts >= 4120) {
    s_status.percent = 100;
  } else {
    s_status.percent = (uint8_t)(((millivolts - 3000U) * 100U) / 1120U);
  }
  return s_status.percent != previousStatus.percent ||
         s_status.charging != previousStatus.charging;
}

BatteryStatus Battery_GetStatus() { return s_status; }
