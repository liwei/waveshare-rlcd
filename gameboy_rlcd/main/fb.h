// Landscape 1 bpp framebuffer and drawing helpers.
//
// Pixels are addressed in panel space: 400 x 300, origin top-left, one byte
// per 8 pixels, MSB leftmost. A *set* bit is paper (light) and a clear bit is
// ink (dark), which is the polarity the ST7305 wants (see st7305.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 5x7 glyph in a 6x8 cell; multiply by the text scale for panel pixels.
#define FB_CHAR_W 6
#define FB_CHAR_H 8

uint8_t *fb_buffer(void);

// Pointer to the first byte of landscape row y (LCD_STRIDE bytes).
uint8_t *fb_row(int y);

void fb_clear(bool ink);
void fb_pixel(int x, int y, bool ink);
bool fb_pixel_get(int x, int y);
void fb_fill_rect(int x, int y, int w, int h, bool ink);
void fb_hline(int x0, int x1, int y, bool ink);
void fb_vline(int x, int y0, int y1, bool ink);

// Draw one glyph and its cell background; returns the advance in pixels.
int fb_char(int x, int y, char ch, int scale, bool text_ink, bool bg_ink);

// Draw a NUL-terminated string; returns the x position after the last glyph.
int fb_text(int x, int y, const char *s, int scale, bool text_ink, bool bg_ink);

int fb_text_width(const char *s, int scale);

// Battery gauge: 16x9 px outline with a proportional fill.
void fb_battery(int x, int y, int percent, bool charging);
