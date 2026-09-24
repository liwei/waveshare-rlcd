#include "rlcd.h"

#include <string.h>

#include "config.h"

// Panel geometry: 38 tiles x 8 px = 304 columns (300 used), 50 tile rows.
#define TILE_WIDTH 38
#define TILE_HEIGHT 50
#define ROW_BYTES (TILE_WIDTH * 8)

// Only one panel exists; the u8g2 callbacks are plain C function pointers and
// cannot carry an instance, so they reach back through this.
static Rlcd *g_lcd = nullptr;

static u8x8_display_info_t panel_info = {
    0,                    // chip_enable_level
    1,                    // chip_disable_level
    0,                    // post_chip_enable_wait_ns
    0,                    // pre_chip_disable_wait_ns
    20,                   // reset_pulse_width_ms
    50,                   // post_reset_wait_ms
    0,                    // sda_setup_time_ns
    0,                    // sck_pulse_width_ns
    RLCD_SPI_HZ,          // sck_clock_hz
    0,                    // spi_mode
    4,                    // i2c_bus_clock_100kHz
    0,                    // data_setup_time_ns
    0,                    // write_pulse_width_ns
    TILE_WIDTH,
    TILE_HEIGHT,
    0,                    // default_x_offset
    0,                    // flip_mode_x_offset
    300,                  // pixel_width
    400,                  // pixel_height
};

Rlcd::Rlcd(int sck, int mosi, int dc, int cs, int rst, uint32_t hz)
    : _dc(dc), _cs(cs), _rst(rst), _hz(hz), _buf(nullptr), _bufSize(0),
      _spi(HSPI) {
  _spi.begin(sck, -1, mosi, -1);
  _spi.beginTransaction(SPISettings(_hz, MSBFIRST, SPI_MODE0));
  g_lcd = this;
}

void Rlcd::cmd(uint8_t c) {
  digitalWrite(_dc, LOW);
  digitalWrite(_cs, LOW);
  _spi.transfer(c);
  digitalWrite(_cs, HIGH);
}

void Rlcd::cmdData(uint8_t c, const uint8_t *d, size_t n) {
  digitalWrite(_dc, LOW);
  digitalWrite(_cs, LOW);
  _spi.transfer(c);
  if (n) {
    digitalWrite(_dc, HIGH);
    _spi.transferBytes((uint8_t *)d, nullptr, n);
  }
  digitalWrite(_cs, HIGH);
}

void Rlcd::hardReset() {
  digitalWrite(_rst, HIGH);
  delay(50);
  digitalWrite(_rst, LOW);
  delay(20);
  digitalWrite(_rst, HIGH);
  delay(50);
}

void Rlcd::panelInit() {
  hardReset();

  const uint8_t d6[] = {0x17, 0x02};
  const uint8_t d1[] = {0x01};
  const uint8_t c0[] = {0x11, 0x04};
  const uint8_t c1[] = {0x69, 0x69, 0x69, 0x69};
  const uint8_t c2[] = {0x19, 0x19, 0x19, 0x19};
  const uint8_t c4[] = {0x4B, 0x4B, 0x4B, 0x4B};
  const uint8_t d8[] = {0x80, 0xE9};
  const uint8_t b2[] = {0x02};
  const uint8_t b3[] = {0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45};
  const uint8_t b4[] = {0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45};
  const uint8_t timing[] = {0x32, 0x03, 0x1F};
  const uint8_t b7[] = {0x13};
  const uint8_t b0[] = {0x64};
  const uint8_t c9[] = {0x00};
  const uint8_t m36[] = {0x48};
  const uint8_t m3a[] = {0x11};
  const uint8_t b9[] = {0x20};
  const uint8_t b8[] = {0x29};
  const uint8_t winA[] = {0x12, 0x2A};
  const uint8_t winB[] = {0x00, 0xC7};
  const uint8_t m35[] = {0x00};
  const uint8_t d0[] = {0xFF};

  cmdData(0xD6, d6, sizeof(d6));
  cmdData(0xD1, d1, sizeof(d1));
  cmdData(0xC0, c0, sizeof(c0));
  cmdData(0xC1, c1, sizeof(c1));
  cmdData(0xC2, c2, sizeof(c2));
  cmdData(0xC4, c4, sizeof(c4));
  cmdData(0xC5, c2, sizeof(c2));
  cmdData(0xD8, d8, sizeof(d8));
  cmdData(0xB2, b2, sizeof(b2));
  cmdData(0xB3, b3, sizeof(b3));
  cmdData(0xB4, b4, sizeof(b4));
  cmdData(0x62, timing, sizeof(timing));
  cmdData(0xB7, b7, sizeof(b7));
  cmdData(0xB0, b0, sizeof(b0));
  cmd(0x11);
  delay(120);
  cmdData(0xC9, c9, sizeof(c9));
  cmdData(0x36, m36, sizeof(m36));
  cmdData(0x3A, m3a, sizeof(m3a));
  cmdData(0xB9, b9, sizeof(b9));
  cmdData(0xB8, b8, sizeof(b8));
  cmd(0x21);
  cmdData(0x2A, winA, sizeof(winA));
  cmdData(0x2B, winB, sizeof(winB));
  cmdData(0x35, m35, sizeof(m35));
  cmdData(0xD0, d0, sizeof(d0));
  cmd(0x38);
  cmd(0x29);
}

uint8_t Rlcd::byteCb(u8x8_t *, uint8_t, uint8_t, void *) {
  // All panel traffic is issued directly from the draw-tile callback.
  return 1;
}

uint8_t Rlcd::gpioCb(u8x8_t *, uint8_t msg, uint8_t arg, void *) {
  switch (msg) {
    case U8X8_MSG_GPIO_CS:
      digitalWrite(g_lcd->_cs, arg ? HIGH : LOW);
      return 1;
    case U8X8_MSG_GPIO_DC:
      digitalWrite(g_lcd->_dc, arg ? HIGH : LOW);
      return 1;
    case U8X8_MSG_GPIO_RESET:
      digitalWrite(g_lcd->_rst, arg ? HIGH : LOW);
      return 1;
    default:
      return 1;
  }
}

uint8_t Rlcd::displayCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg, void *ptr) {
  switch (msg) {
    case U8X8_MSG_DISPLAY_SETUP_MEMORY:
      u8x8_d_helper_display_setup_memory(u8x8, &panel_info);
      return 1;

    case U8X8_MSG_DISPLAY_INIT:
      g_lcd->panelInit();
      return 1;

    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
      g_lcd->cmd(arg == 0 ? 0x29 : 0x28);
      return 1;

    case U8X8_MSG_DISPLAY_DRAW_TILE: {
      u8x8_tile_t *tile = (u8x8_tile_t *)ptr;
      const uint8_t cnt = tile->cnt;
      const uint8_t x_pos = tile->x_pos;
      const uint8_t y_pos = tile->y_pos;

      // Columns covered by this tile run.
      int first_col = x_pos * 8;
      int last_col = (x_pos + cnt) * 8 - 1;
      if (last_col >= 300) {
        last_col = 299;
      }

      // The panel packs 12 columns into one address step and stores a column
      // group in 3 bytes per sub-row, hence the /12 and *3 below.
      const int addr_start = 0x12 + first_col / 12;
      const int addr_end = 0x12 + last_col / 12;
      const int send_start = (addr_start - 0x12) * 3;
      const int send_cnt = (addr_end - addr_start + 1) * 3;

      int addr_first_col = (addr_start - 0x12) * 12;
      int addr_last_col = (addr_end - 0x12) * 12 + 11;
      if (addr_last_col >= 300) {
        addr_last_col = 299;
      }

      // Start of the first row of this tile run (tile_ptr points at x_pos).
      uint8_t *row_base = tile->tile_ptr - (uint16_t)x_pos * 8U;

      const uint8_t col_bounds[] = {(uint8_t)(0x3C - addr_end),
                                    (uint8_t)(0x3C - addr_start)};
      const uint8_t row_bounds[] = {(uint8_t)(y_pos * 4),
                                    (uint8_t)(y_pos * 4 + 3)};

      // 1 bpp -> the panel's packed column-group format.
      //
      // The ST7305 lights a pixel for a set bit, but U8g2's framebuffer uses a
      // set bit for *ink*. Waveshare's own U8g2 demo (which draws with colour 1)
      // renders as light-on-dark, so each entry below is the reference table
      // with its row reversed, i.e. the transmitted pixel polarity flipped.
      // This also makes the night-mode buffer fill come out as a dark screen.
      static const uint8_t lut[4][4] = {
          {0xC0, 0x40, 0x80, 0x00},
          {0x30, 0x10, 0x20, 0x00},
          {0x0C, 0x04, 0x08, 0x00},
          {0x03, 0x01, 0x02, 0x00},
      };

      uint8_t rows[300] = {0};
      for (int sr = 0; sr < 4; sr++) {
        const int shift = sr * 2;
        const int idx0 = sr * send_cnt + (addr_first_col >> 2) - send_start;
        int idx = idx0;
        for (int col = addr_first_col; col <= addr_last_col; col += 4, idx++) {
          rows[idx] = lut[0][(row_base[col] >> shift) & 3] |
                      lut[1][(row_base[col + 1] >> shift) & 3] |
                      lut[2][(row_base[col + 2] >> shift) & 3] |
                      lut[3][(row_base[col + 3] >> shift) & 3];
        }
      }

      g_lcd->cmdData(0x2A, col_bounds, sizeof(col_bounds));
      g_lcd->cmdData(0x2B, row_bounds, sizeof(row_bounds));
      g_lcd->cmdData(0x2C, rows, (size_t)send_cnt * 4U);
      return 1;
    }

    default:
      return 0;
  }
}

bool Rlcd::begin(const u8g2_cb_t *rotation) {
  pinMode(_dc, OUTPUT);
  pinMode(_cs, OUTPUT);
  pinMode(_rst, OUTPUT);
  digitalWrite(_cs, HIGH);
  digitalWrite(_dc, HIGH);
  digitalWrite(_rst, HIGH);

  _bufSize = (size_t)ROW_BYTES * TILE_HEIGHT;
  _buf = (uint8_t *)malloc(_bufSize);
  if (!_buf) {
    return false;
  }
  memset(_buf, 0, _bufSize);

  u8x8_Setup(u8g2_GetU8x8(&_g), displayCb, u8x8_dummy_cb, byteCb, gpioCb);
  u8g2_SetupBuffer(&_g, _buf, TILE_HEIGHT, u8g2_ll_hvline_vertical_top_lsb,
                   rotation);
  u8g2_InitDisplay(&_g);
  u8g2_SetPowerSave(&_g, 0);
  return true;
}
