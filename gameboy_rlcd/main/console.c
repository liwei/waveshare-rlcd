#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"
#include "buttons.h"
#include "config.h"
#include "fb.h"
#include "gbemu.h"
#include "input.h"
#include "input_bt.h"
#include "minigb_apu/minigb_apu.h"
#include "st7305.h"
#include "ui.h"

static const char *TAG = "console";

static volatile int64_t s_frame_us;
static volatile int64_t s_emu_us;
static volatile int64_t s_flush_us;
static volatile int64_t s_audio_us;
static volatile int s_frames;
static volatile int s_groups;
static int64_t s_next_report_us;
static int s_frame_skip = FRAME_SKIP_AUTO;

// All console writes are bounded: a host that stops reading must never stall
// the emulator, and a partial dump is simply abandoned.
#define CONSOLE_WRITE_TIMEOUT pdMS_TO_TICKS(50)

static bool write_bytes(const void *data, size_t len) {
  return usb_serial_jtag_write_bytes(data, len, CONSOLE_WRITE_TIMEOUT) == len;
}

static void write_str(const char *s) { write_bytes(s, strlen(s)); }

static void write_fmt(const char *fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n > 0) {
    write_bytes(buf, (size_t)((n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1));
  }
}

static void dump_framebuffer(void) {
  const uint8_t *fb = fb_buffer();
  write_fmt("FBUF %d\n", LCD_FB_SIZE);

  static char line[LCD_STRIDE * 2 + 2];
  for (int y = 0; y < LCD_H; y++) {
    const uint8_t *row = &fb[y * LCD_STRIDE];
    for (int i = 0; i < LCD_STRIDE; i++) {
      static const char hex[] = "0123456789abcdef";
      line[i * 2] = hex[row[i] >> 4];
      line[i * 2 + 1] = hex[row[i] & 0xF];
    }
    line[LCD_STRIDE * 2] = '\n';
    line[LCD_STRIDE * 2 + 1] = '\0';
    if (!write_bytes(line, LCD_STRIDE * 2 + 1)) {
      return; /* host went away mid-dump */
    }
  }

  write_str("FBUF-END\n");
}

static void status_line(void) {
  uint32_t rx_total;
  uint32_t rx_buttons;
  input_bt_rx_stats(&rx_total, &rx_buttons);

  write_fmt(
      "state=%s mode=%s menu=%d bt=%s pad=0x%02x buttons=0x%02x rx=%u/%u engine=%s rate=%d "
      "vol=%d ring=%d underruns=%u heap=%uKB\n",
      paperboy_gb_is_ready() ? "ready" : "idle", paperboy_gb_is_cgb() ? "CGB" : "DMG",
      ui_menu_active() ? 1 : 0, input_bt_connected() ? input_bt_name() : "-",
      input_bt_buttons(), paperboy_gb_buttons(), (unsigned)rx_total, (unsigned)rx_buttons,
      audio_engine_name(audio_get_engine()), AUDIO_SAMPLE_RATE, audio_get_volume(),
      audio_ring_used(), (unsigned)audio_underrun_count(),
      (unsigned)(esp_get_free_heap_size() / 1024));
}

int console_frame_skip_mode(void) { return s_frame_skip; }

void console_init(void) {
  usb_serial_jtag_driver_config_t cfg = {
      .tx_buffer_size = 4096,
      .rx_buffer_size = 2048,
  };
  const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "driver install: %s", esp_err_to_name(err));
  }
  s_next_report_us = esp_timer_get_time() + 5000000;
}

void console_frame(int64_t total_us, int64_t emu_us, int64_t flush_us, int64_t audio_us, int g0,
                   int g1) {
  s_frame_us += total_us;
  s_emu_us += emu_us;
  s_flush_us += flush_us;
  s_audio_us += audio_us;
  s_frames++;
  if (g1 >= g0) {
    s_groups += g1 - g0 + 1;
  }
}

void console_poll(void) {
  static uint8_t inject;
  static bool s_verbose;

  uint8_t rx[64];
  const int got = usb_serial_jtag_read_bytes(rx, sizeof(rx), 0);
  for (int i = 0; i < got; i++) {
    switch (rx[i]) {
      case 'd':
        dump_framebuffer();
        break;
      case 's':
        status_line();
        break;
      case 'g': {
        char state[256];
        paperboy_gb_dump_state(state, sizeof(state));
        write_fmt("%s\n", state);
        break;
      }
      case 'F': {
        // Full repaint, timed: repacking the dithered framebuffer versus
        // shifting it out over SPI.
        int64_t pack_us = 0;
        int64_t spi_us = 0;
        rlcd_flush_all(fb_buffer());
        rlcd_last_flush_timing(&pack_us, &spi_us);
        write_fmt("full repaint: %d groups, pack %lld us, spi %lld us, total %lld us\n",
                  PANEL_GROUPS, pack_us, spi_us, pack_us + spi_us);
        break;
      }
      case 'p':
        s_verbose = !s_verbose;
        input_bt_set_verbose(s_verbose);
        break;
      case 'k':
        inject |= BTN_KEY;
        buttons_override(inject, inject);
        write_str("key down\n");
        break;
      case 'u':
        inject &= (uint8_t)~BTN_KEY;
        buttons_override(inject, inject);
        write_str("key up\n");
        break;
      case 'b':
        inject |= BTN_BOOT;
        buttons_override(inject, inject);
        write_str("boot down\n");
        break;
      case 'n':
        inject = 0;
        buttons_override(0, 0);
        write_str("released\n");
        break;
      case 'N':
        inject = 0;
        buttons_override(0, 0); /* hand control back to the pins */
        write_str("hardware buttons\n");
        break;
      case 'B':
        input_bt_scan_start(8);
        write_str("scan started\n");
        break;
      case 'l': {
        esp_hid_scan_result_t *results = NULL;
        const size_t count = input_bt_scan_results(&results);
        write_fmt("scan state=%d devices=%u\n", input_bt_scan_state(), (unsigned)count);
        size_t i = 0;
        for (const esp_hid_scan_result_t *r = results; r != NULL; r = r->next, i++) {
          write_fmt("  [%u] %s rssi=%d usage=%s addr=%02x:%02x:%02x:%02x:%02x:%02x type=%d\n", (unsigned)i,
                    (r->name != NULL) ? r->name : "(unnamed)", r->rssi,
                    esp_hid_usage_str(r->usage), r->bda[0], r->bda[1], r->bda[2], r->bda[3],
                    r->bda[4], r->bda[5], r->ble.addr_type);
        }
        break;
      }
      case 'C': {
        esp_hid_scan_result_t *results = NULL;
        const size_t count = input_bt_scan_results(&results);
        if (count == 0 || results == NULL) {
          write_str("no scan results; press B first\n");
        } else {
          input_bt_connect(results);
          write_str("connecting\n");
        }
        break;
      }
      case 'c':
        input_bt_forget();
        write_str("bond forgotten\n");
        break;
      case 'j': {
        // Step through the pad's button indices, one per press of this key, so
        // the mapping page can be exercised with no pad in hand.
        static int next_index;
        input_bt_test_press(next_index);
        write_fmt("injected pad button %d\n", next_index);
        next_index = (next_index + 1) % 16;
        break;
      }
      case 't': {
        // Pull a trigger through the same path a real report takes: R2 (quick
        // save) then L2 (quick load), alternating, so both can be exercised
        // without the pad in hand.
        static bool right;
        const int index = right ? 33 : 34;
        input_bt_test_press(index);
        write_fmt("pulled %s\n", right ? "R2 (quick save)" : "L2 (quick load)");
        right = !right;
        break;
      }
      case 'v': {
        size_t len = 0;
        const uint8_t *desc = input_bt_descriptor(&len);
        write_fmt("descriptor: %u bytes\n", (unsigned)len);
        for (size_t i = 0; i < len; i += 16) {
          char line[3 * 16 + 1];
          size_t n = 0;
          for (size_t b = i; b < len && b < i + 16; b++) {
            n += (size_t)snprintf(line + n, sizeof(line) - n, "%02x", desc[b]);
          }
          write_fmt("  %s\n", line);
        }
        break;
      }
      case 'i': {
        static const char *const names[PAD_ACTION_COUNT] = {
            "A", "B", "Start", "Select", "Up", "Down", "Left", "Right", "Menu"};
        write_fmt("button map (%s):\n", input_bt_connected() ? input_bt_name() : "no pad, default");
        for (int i = 0; i < PAD_ACTION_COUNT; i++) {
          const int8_t index = input_bt_action_get(i);
          if (index < 0) {
            write_fmt("  %-7s not set\n", names[i]);
          } else {
            write_fmt("  %-7s idx %2d  %s\n", names[i], index, input_bt_index_name(index));
          }
        }
        break;
      }
      case 'x':
        s_frame_skip = (s_frame_skip + 1) % 3;
        write_fmt("frame skip: %s\n", s_frame_skip == FRAME_SKIP_AUTO      ? "auto"
                                     : s_frame_skip == FRAME_SKIP_NEVER   ? "never"
                                                                          : "alternate");
        break;
      case 'f':
        if (s_frames > 0) {
          write_fmt("frames=%d avg total=%.2fms emu=%.2fms flush=%.2fms audio=%.2fms groups=%d\n",
                    s_frames, (double)s_frame_us / s_frames / 1000.0,
                    (double)s_emu_us / s_frames / 1000.0,
                    (double)s_flush_us / s_frames / 1000.0,
                    (double)s_audio_us / s_frames / 1000.0, s_groups / s_frames);
        }
        s_frames = 0;
        s_frame_us = 0;
        s_emu_us = 0;
        s_flush_us = 0;
        s_audio_us = 0;
        s_groups = 0;
        break;
      default:
        break;
    }
  }

  const int64_t now = esp_timer_get_time();
  if (now >= s_next_report_us && s_frames > 0) {
    write_fmt("hb frames=%d avg=%.2fms emu=%.2fms flush=%.2fms audio=%.2fms groups=%d\n",
              s_frames, (double)s_frame_us / s_frames / 1000.0,
              (double)s_emu_us / s_frames / 1000.0,
              (double)s_flush_us / s_frames / 1000.0,
              (double)s_audio_us / s_frames / 1000.0, s_groups / s_frames);
    s_frames = 0;
    s_frame_us = 0;
    s_emu_us = 0;
    s_flush_us = 0;
    s_audio_us = 0;
    s_groups = 0;
    s_next_report_us = now + 5000000;
  }
}
