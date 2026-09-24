// ST7305 300x400 reflective LCD driver.
//
// The init sequence and the panel's packed RAM layout (12-pixel column groups
// addressed in mirrored order, 2 rows per row-unit) follow Waveshare's
// reference driver as ported and verified on hardware in dino_rlcd/rlcd.cpp.
//
// Unlike that driver this one owns its framebuffer layout instead of borrowing
// U8g2's, so the tile conversion is replaced by a direct band packer.
#include "st7305.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"

static const char *TAG = "st7305";

static spi_device_handle_t s_spi;

// Whole-frame staging: the panel walks its row address across the whole
// window, so one 0x2C transfer carries every band.
static uint8_t s_stage[PANEL_GROUPS * 3 * 4 * PANEL_BANDS] __attribute__((aligned(4)));

static inline void cs_set(int level) { gpio_set_level(RLCD_CS_PIN, level); }
static inline void dc_set(int level) { gpio_set_level(RLCD_DC_PIN, level); }

static void tx(const uint8_t *data, size_t n) {
  spi_transaction_t t = {
      .length = n * 8,
      .tx_buffer = data,
  };
  ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void write_cmd(uint8_t c) {
  dc_set(0);
  tx(&c, 1);
}

static void write_data(const uint8_t *d, size_t n) {
  if (n == 0) {
    return;
  }
  dc_set(1);
  tx(d, n);
}

static void cmd_data(uint8_t c, const uint8_t *d, size_t n) {
  cs_set(0);
  write_cmd(c);
  write_data(d, n);
  cs_set(1);
}

static void hard_reset(void) {
  gpio_set_level(RLCD_RST_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(50));
  gpio_set_level(RLCD_RST_PIN, 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(RLCD_RST_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(50));
}

static void panel_init(void) {
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
  const uint8_t win_a[] = {0x12, 0x2A};
  const uint8_t win_b[] = {0x00, 0xC7};
  const uint8_t m35[] = {0x00};
  const uint8_t d0[] = {0xFF};

  hard_reset();

  cmd_data(0xD6, d6, sizeof(d6));
  cmd_data(0xD1, d1, sizeof(d1));
  cmd_data(0xC0, c0, sizeof(c0));
  cmd_data(0xC1, c1, sizeof(c1));
  cmd_data(0xC2, c2, sizeof(c2));
  cmd_data(0xC4, c4, sizeof(c4));
  cmd_data(0xC5, c2, sizeof(c2));
  cmd_data(0xD8, d8, sizeof(d8));
  cmd_data(0xB2, b2, sizeof(b2));
  cmd_data(0xB3, b3, sizeof(b3));
  cmd_data(0xB4, b4, sizeof(b4));
  cmd_data(0x62, timing, sizeof(timing));
  cmd_data(0xB7, b7, sizeof(b7));
  cmd_data(0xB0, b0, sizeof(b0));
  cmd_data(0x11, NULL, 0);
  vTaskDelay(pdMS_TO_TICKS(120));
  cmd_data(0xC9, c9, sizeof(c9));
  cmd_data(0x36, m36, sizeof(m36));
  cmd_data(0x3A, m3a, sizeof(m3a));
  cmd_data(0xB9, b9, sizeof(b9));
  cmd_data(0xB8, b8, sizeof(b8));
  cmd_data(0x21, NULL, 0);
  cmd_data(0x2A, win_a, sizeof(win_a));
  cmd_data(0x2B, win_b, sizeof(win_b));
  cmd_data(0x35, m35, sizeof(m35));
  cmd_data(0xD0, d0, sizeof(d0));
  cmd_data(0x38, NULL, 0);
  cmd_data(0x29, NULL, 0);
}

// Panel RAM packs a group of 12 columns and 2 rows into three bytes, four
// columns per byte, with the earlier row in the higher of each pixel's two
// bits. The panel lights a pixel for a *clear* bit, and the framebuffer uses a
// set bit for paper, so the two agree without an inversion.
//
// A framebuffer byte holds eight panel rows, which is two rows for each of the
// four row-units in a band, so one byte fans out into four sub-row updates.
// s_pair precomputes that fan-out: s_pair[byte][sub-row][column-in-group].
static uint8_t s_pair[256][4][4];

static void build_pair_table(void) {
  for (int v = 0; v < 256; v++) {
    for (int sr = 0; sr < 4; sr++) {
      const uint8_t upper = (uint8_t)((v >> (7 - 2 * sr)) & 1u);
      const uint8_t lower = (uint8_t)((v >> (6 - 2 * sr)) & 1u);
      for (int i = 0; i < 4; i++) {
        s_pair[v][sr][i] =
            (uint8_t)((upper << (7 - 2 * i)) | (lower << (6 - 2 * i)));
      }
    }
  }
}

// Timing of the most recent flush, split into repacking and SPI transfer, so
// the cost of a full repaint can be attributed rather than guessed at.
static volatile int64_t s_pack_us;
static volatile int64_t s_spi_us;

void rlcd_last_flush_timing(int64_t *pack_us, int64_t *spi_us) {
  if (pack_us != NULL) {
    *pack_us = s_pack_us;
  }
  if (spi_us != NULL) {
    *spi_us = s_spi_us;
  }
}

void rlcd_flush_groups(const uint8_t *fb, int g0, int g1) {
  if (g0 < 0) {
    g0 = 0;
  }
  if (g1 > PANEL_GROUPS - 1) {
    g1 = PANEL_GROUPS - 1;
  }
  if (g0 > g1) {
    return;
  }

  const int send_cnt = (g1 - g0 + 1) * 3;

  // Payload order is band, then sub-row, then group: the panel increments the
  // row address across the window and expects each row-unit's groups in turn.
  const int64_t pack_start = esp_timer_get_time();
  memset(s_stage, 0, (size_t)send_cnt * 4u * PANEL_BANDS);

  for (int y = 0; y < PANEL_COLS; y++) {
    // Landscape row y is panel column 299 - y.
    const int col = (PANEL_COLS - 1) - y;
    const int group = col / 12;
    if (group < g0 || group > g1) {
      continue;
    }
    const int byte_in_group = (col % 12) / 4;
    const int slot = col % 4;
    const uint8_t *row = &fb[y * LCD_STRIDE];
    uint8_t *base = &s_stage[(group - g0) * 3 + byte_in_group];

    for (int band = 0; band < PANEL_BANDS; band++) {
      const uint8_t *fan = s_pair[row[band]][0];
      uint8_t *out = base + band * send_cnt * 4;
      out[0 * send_cnt] |= fan[slot];
      out[1 * send_cnt] |= fan[slot + 4];
      out[2 * send_cnt] |= fan[slot + 8];
      out[3 * send_cnt] |= fan[slot + 12];
    }
  }

  const uint8_t col_bounds[2] = {
      (uint8_t)(0x3C - (0x12 + g1)),
      (uint8_t)(0x3C - (0x12 + g0)),
  };
  const size_t payload = (size_t)send_cnt * 4u * PANEL_BANDS;

  s_pack_us = esp_timer_get_time() - pack_start;
  const int64_t spi_start = esp_timer_get_time();

#if RLCD_ONE_WINDOW
  // One column window and one full-height row window, then the whole payload.
  const uint8_t row_bounds[2] = {0x00, 0xC7};

  cs_set(0);
  write_cmd(0x2A);
  write_data(col_bounds, sizeof(col_bounds));
  write_cmd(0x2B);
  write_data(row_bounds, sizeof(row_bounds));
  write_cmd(0x2C);
  write_data(s_stage, payload);
  cs_set(1);
#else
  // Conservative path: re-arm the row window for every 8-row band, which is
  // what Waveshare's reference driver does. Costs 50 window setups per flush.
  cs_set(0);
  write_cmd(0x2A);
  write_data(col_bounds, sizeof(col_bounds));
  cs_set(1);

  for (int band = 0; band < PANEL_BANDS; band++) {
    const uint8_t row_bounds[2] = {
        (uint8_t)(band * 4),
        (uint8_t)(band * 4 + 3),
    };

    cs_set(0);
    write_cmd(0x2B);
    write_data(row_bounds, sizeof(row_bounds));
    write_cmd(0x2C);
    write_data(&s_stage[(size_t)band * send_cnt * 4u], (size_t)send_cnt * 4u);
    cs_set(1);
  }
#endif

  s_spi_us = esp_timer_get_time() - spi_start;
}

void rlcd_flush_all(const uint8_t *fb) {
  rlcd_flush_groups(fb, 0, PANEL_GROUPS - 1);
}

void rlcd_set_power(bool on) {
  cs_set(0);
  write_cmd(on ? 0x29 : 0x28);
  cs_set(1);
}

bool rlcd_init(void) {
  build_pair_table();

  const gpio_config_t rst_cfg = {
      .pin_bit_mask = (1ULL << RLCD_RST_PIN) | (1ULL << RLCD_DC_PIN) | (1ULL << RLCD_CS_PIN),
      .mode = GPIO_MODE_OUTPUT,
  };
  ESP_ERROR_CHECK(gpio_config(&rst_cfg));
  gpio_set_level(RLCD_CS_PIN, 1);
  gpio_set_level(RLCD_DC_PIN, 1);
  gpio_set_level(RLCD_RST_PIN, 1);

  const spi_bus_config_t bus = {
      .mosi_io_num = RLCD_MOSI_PIN,
      .miso_io_num = -1,
      .sclk_io_num = RLCD_SCK_PIN,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = sizeof(s_stage),
  };
  // DMA is required: without it the driver caps a transaction at 64 bytes and
  // a band payload is 300.
  esp_err_t err = spi_bus_initialize(RLCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return false;
  }

  // CS and DC are driven by hand so a command byte and its parameters stay in
  // one chip-select window.
  const spi_device_interface_config_t dev = {
      .clock_speed_hz = RLCD_SPI_HZ,
      .mode = 0,
      .spics_io_num = -1,
      .queue_size = 1,
  };
  err = spi_bus_add_device(RLCD_SPI_HOST, &dev, &s_spi);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
    return false;
  }

  panel_init();
  ESP_LOGI(TAG, "ST7305 ready: %dx%d panel, %d groups x %d bands", PANEL_COLS, PANEL_ROWS,
           PANEL_GROUPS, PANEL_BANDS);
  return true;
}
