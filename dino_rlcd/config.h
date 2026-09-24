// Board and display configuration for the Waveshare ESP32-S3-RLCD-4.2.
#pragma once

#include <stdint.h>

// ---- Pin map (from Waveshare's example projects) --------------------------
#define RLCD_SCK_PIN 11
#define RLCD_MOSI_PIN 12
#define RLCD_DC_PIN 5
#define RLCD_CS_PIN 40
#define RLCD_RST_PIN 41

#define BTN_KEY_PIN 18   // "KEY"  button, active low
#define BTN_BOOT_PIN 0   // "BOOT" button, active low
#define BATTERY_ADC_PIN 4
#define BATTERY_SAMPLE_INTERVAL_MS 10000
#define BATTERY_CHARGE_RISE_MV 25
#define BATTERY_CHARGE_SAMPLES 3

// ---- Panel ----------------------------------------------------------------
// The ST7305 is a 300x400 reflective panel; U8G2_R1 rotates it to landscape.
#define LCD_W 400
#define LCD_H 300
#define RLCD_SPI_HZ 24000000

// Simulation runs on a fixed 60 Hz tick, matching Chromium's FPS constant.
#define TICK_US 16667
#define MS_PER_FRAME 16.67f
