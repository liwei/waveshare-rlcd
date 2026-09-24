// Battery gauge on ADC1 channel 3 (GPIO4) behind the board's 100k/200k divider.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void battery_init(void);

// Resamples at most once per BATTERY_SAMPLE_INTERVAL_MS.
void battery_update(void);

bool battery_valid(void);
uint32_t battery_millivolts(void);
int battery_percent(void);
bool battery_charging(void);
bool battery_is_low(void);
