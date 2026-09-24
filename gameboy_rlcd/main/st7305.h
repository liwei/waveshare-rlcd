// ST7305 300x400 reflective LCD on the Waveshare ESP32-S3-RLCD-4.2.
//
// The panel is 1 bit per pixel and is used rotated 90 degrees, so the caller's
// framebuffer is landscape 400x300 (see fb.h). A flush writes whole column
// groups (12 panel columns) across every band (8 panel rows).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Brings up SPI, resets the panel and runs the init sequence.
bool rlcd_init(void);

// Push panel column groups [g0, g1] (inclusive, 0..24) from the landscape
// framebuffer. Group g holds landscape rows [288 - 12*g, 299 - 12*g].
void rlcd_flush_groups(const uint8_t *fb, int g0, int g1);

// Push the whole framebuffer.
void rlcd_flush_all(const uint8_t *fb);

// Display on/off (0x29 / 0x28).
void rlcd_set_power(bool on);

// Split of the most recent flush into repacking and SPI transfer, in
// microseconds. Either pointer may be NULL.
void rlcd_last_flush_timing(int64_t *pack_us, int64_t *spi_us);
