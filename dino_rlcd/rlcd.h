// U8g2 display driver for the ST7305 300x400 reflective LCD used on the
// Waveshare ESP32-S3-RLCD-4.2.
//
// U8g2 only owns the framebuffer here; every byte that reaches the panel is
// pushed directly over SPI, following the panel init sequence and the tile
// -> panel-row conversion from Waveshare's reference driver.
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

class Rlcd {
 public:
  Rlcd(int sck, int mosi, int dc, int cs, int rst, uint32_t hz);

  // Allocates the framebuffer and initialises the panel.
  bool begin(const u8g2_cb_t *rotation = U8G2_R1);

  u8g2_t *gfx() { return &_g; }
  uint8_t *buffer() { return _buf; }
  size_t bufferSize() const { return _bufSize; }

  void flush() { u8g2_SendBuffer(&_g); }

 private:
  static uint8_t byteCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg, void *ptr);
  static uint8_t gpioCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg, void *ptr);
  static uint8_t displayCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg, void *ptr);

  void cmd(uint8_t c);
  void cmdData(uint8_t c, const uint8_t *d, size_t n);
  void hardReset();
  void panelInit();

  int _dc, _cs, _rst;
  uint32_t _hz;
  uint8_t *_buf;
  size_t _bufSize;
  u8g2_t _g;
  SPIClass _spi;
};
