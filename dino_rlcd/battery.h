#pragma once

#include <stdint.h>

struct BatteryStatus {
  uint8_t percent;
  bool charging;
};

void Battery_Init();
bool Battery_Update();
BatteryStatus Battery_GetStatus();
