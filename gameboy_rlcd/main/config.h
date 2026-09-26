// Board, panel and tuning configuration for the Waveshare ESP32-S3-RLCD-4.2.
#pragma once

#include <stdint.h>

// ---- RLCD panel: ST7305 300x400 reflective LCD on SPI2 --------------------
#define RLCD_SCK_PIN 11
#define RLCD_MOSI_PIN 12
#define RLCD_DC_PIN 5
#define RLCD_CS_PIN 40
#define RLCD_RST_PIN 41
#define RLCD_SPI_HOST SPI2_HOST
// Waveshare's reference driver and this board's dino_rlcd sketch both run the
// panel at 24 MHz. It has been pushed higher here because the flush is on the
// critical path of every frame; drop it back if the panel shows artefacts.
#define RLCD_SPI_HZ (40 * 1000 * 1000)

// Arm the row window once and stream every band in a single 0x2C write, rather
// than re-arming it for each of the 50 bands. Saves ~150 SPI transactions per
// flush, at the cost of relying on the panel's row address auto-incrementing
// across the whole window (200 row-units = the panel's full 400 rows).
#define RLCD_ONE_WINDOW 1

// The panel is natively portrait; it is addressed as 25 column groups of 12
// pixels by 50 bands of 8 rows, and only ever written a whole band at a time.
#define PANEL_COLS 300
#define PANEL_ROWS 400
#define PANEL_GROUPS (PANEL_COLS / 12)
#define PANEL_BANDS (PANEL_ROWS / 8)

// ---- Landscape geometry ---------------------------------------------------
// The panel is used rotated 90 degrees, exactly as the dino_rlcd sketch does
// with U8G2_R1: landscape (x, y) maps to panel (col = 299 - y, row = x).
#define LCD_W 400
#define LCD_H 300
#define LCD_STRIDE (LCD_W / 8)
#define LCD_FB_SIZE (LCD_STRIDE * LCD_H)

// Game Boy picture: 2x scale, 160x144 -> 320x288, with a 40 px side margin.
#define GB_SCALE 2
#define GB_ORIGIN_X 40
#define GB_ORIGIN_Y 0
#define GB_IMAGE_W (160 * GB_SCALE)
#define GB_IMAGE_H (144 * GB_SCALE)

// Bottom status strip: exactly one panel column group tall (12 px).
#define STATUS_Y GB_IMAGE_H
#define STATUS_H (LCD_H - GB_IMAGE_H)

// ---- Buttons --------------------------------------------------------------
#define BTN_KEY_PIN 18
#define BTN_BOOT_PIN 0
#define BTN_CHORD_MS 800

// Two buttons pressed "together" are never pressed in the same poll. For this
// long after one goes down the other may still join it, so a chord that is a few
// tens of milliseconds out is still a chord rather than the first button's own
// meaning - which on some screens is destructive.
#define BTN_CHORD_GRACE_MS 60

// ---- Battery: ADC1 channel 3 = GPIO4 behind a 100k/200k divider -----------
#define BATTERY_ADC_UNIT ADC_UNIT_1
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_3
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12
#define BATTERY_DIVIDER_SCALE 3
#define BATTERY_SAMPLE_INTERVAL_MS 10000
#define BATTERY_LOW_MV 3400
#define BATTERY_FULL_MV 4120

// ---- TF card on the SDMMC host, 1-bit bus --------------------------------
#define SD_CLK_PIN 38
#define SD_CMD_PIN 21
#define SD_D0_PIN 39
#define SD_MOUNT_POINT "/sdcard"
#define ROM_MAX_SIZE (4 * 1024 * 1024)

// ---- Audio: ES8311 codec on I2S0 -----------------------------------------
// The sample rate and per-frame chunk size live in minigb_apu.h, which is the
// single source of truth for them.
#define AUDIO_VOLUME_DEFAULT 70
// I2C port the codec, RTC and sensor sit on (SDA 13, SCL 14).
#define CODEC_I2C_PORT 0

// ---- Timing ---------------------------------------------------------------
// One Game Boy frame is 70224 cycles at 4.194304 MHz.
#define GB_FRAME_US 16742

// Frame budget the profiler reports against, and the window for its
// long-running average-CPU summary (0 disables that summary).
#define PROF_BUDGET_MS 16.74f
#define PAPERBOY_PROF_SPEED_WINDOW_MS 120000u
