// Game Boy core wrapper: vendored from PaperBoy (zephray/paperboy @ e70b293)
// and re-targeted from its 960x540 e-paper "3 sub-pixels per pixel" layout to
// this board's 400x300 1 bpp reflective LCD.
//
// The panel has no greyscale, so each 160x144 Game Boy pixel becomes a 2x2
// block of panel pixels whose 2x2 ordered pattern encodes the original shade.
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"

#include "audio.h"
#include "config.h"
#include "fb.h"
#include "gbemu.h"
#include "profiler.h"

#define PGB_CGB 0
#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define PGB_IMPL

uint8_t audio_read(void *audio, const uint16_t addr);
void audio_write(void *audio, const uint16_t addr, const uint8_t val);

#include "crankboy_core/peanut_gb.h"

static const char *TAG = "gbemu";

int preferences_cgb_speed;
int preferences_ppu_timing;
int audio_enabled;

static gb_s s_gb;
static uint8_t *s_rom;
static size_t s_rom_size;
static uint8_t *s_cart_ram;
static size_t s_cart_ram_size;
static uint8_t s_lcd[LCD_BUFFER_BYTES] __attribute__((aligned(32)));
static uint8_t s_previous_lcd[LCD_BUFFER_BYTES] __attribute__((aligned(32)));
static uint8_t s_wram[WRAM_SIZE_CGB] __attribute__((aligned(32)));
static uint8_t s_vram[VRAM_SIZE_CGB] __attribute__((aligned(32)));
static bool s_ready;
static char s_last_error[128];

// Video mode requested by the front end. The core only takes this at init, so
// changing it means re-initialising; it ends up in gb_s.is_cgb_mode, which is
// also what a loaded save state restores, so the picture always follows the
// state the machine is actually in.
static bool s_cgb_preference;

// Map an emulator shade onto the two sub-pixel rows of its 2x2 block. Bit 1 of
// each entry is the left sub-pixel, bit 0 the right.
//
// The core stores the palette-mapped shade, where shade 0 is the *lightest*
// level and 3 the darkest (BG_REMAP in peanut_gb_core.h applies BGP, and a
// stock 0xE4 palette maps colour 0 to 0 and colour 3 to 3). Paperboy's ui.h
// documents the opposite convention, but its dither map is inverted to suit
// the M5PaperS3 e-paper panel.
static const uint8_t kDither[4][2] = {
    {0x3, 0x3},  // shade 0, lightest
    {0x1, 0x3},  // 3/4
    {0x2, 0x1},  // 1/2, on a diagonal
    {0x0, 0x0},  // shade 3, darkest
};

struct persist_header {
  uint8_t magic[4];
  uint8_t version;
  uint8_t has_cart_ram;
  uint8_t has_rtc;
  uint8_t reserved;
  uint32_t cart_ram_size;
  uint32_t timestamp;
};

#define PERSIST_VERSION 1u
static const uint8_t PERSIST_MAGIC[4] = {'P', 'B', 'S', 'V'};

uint8_t audio_read(void *audio, const uint16_t addr) {
  (void)audio;
  return audio_apu_read(addr);
}

void audio_write(void *audio, const uint16_t addr, const uint8_t val) {
  (void)audio;
  audio_apu_write(addr, val);
}

void __gb_on_breakpoint(gb_s *gb, int breakpoint_number) {
  (void)gb;
  (void)breakpoint_number;
}

static void set_last_error(const char *msg) {
  strlcpy(s_last_error, msg, sizeof(s_last_error));
}

static bool gb_has_persist_data(void) {
  return s_cart_ram_size > 0 || s_gb.cart_battery;
}

static void gb_invalidate_video_cache(void) {
  for (size_t index = 0; index < sizeof(s_previous_lcd); index++) {
    s_previous_lcd[index] = (uint8_t)~s_lcd[index];
  }
}

static size_t gb_persist_rtc_size(void) {
  return s_gb.cart_battery ? sizeof(s_gb.cart_rtc) : 0u;
}

static inline uint8_t gb_lcd_get_pixel(const uint8_t *lcd, unsigned x, unsigned y) {
  const uint8_t packed = lcd[(y * LCD_WIDTH_PACKED) + (x >> 2)];
  const unsigned shift = (x & 0x3u) << 1;

  return (packed >> shift) & 0x03u;
}

// Expand one Game Boy scanline into two panel rows of 2x2 dithered blocks.
static void blit_line(unsigned line) {
  uint8_t *top = fb_row(GB_ORIGIN_Y + (int)line * GB_SCALE) + (GB_ORIGIN_X / 8);
  uint8_t *bottom = top + LCD_STRIDE;
  const uint8_t *src = &s_lcd[line * LCD_WIDTH_PACKED];

  for (unsigned b = 0; b < LCD_WIDTH_PACKED; b++) {
    const uint8_t packed = src[b];
    uint8_t upper = 0;
    uint8_t lower = 0;

    for (int k = 0; k < 4; k++) {
      const int shade = (packed >> (2 * k)) & 0x3;
      upper |= (uint8_t)(kDither[shade][0] << (6 - 2 * k));
      lower |= (uint8_t)(kDither[shade][1] << (6 - 2 * k));
    }

    top[b] = upper;
    bottom[b] = lower;
  }
}

static void gb_error_cb(gb_s *gb, const enum gb_error_e err, const uint16_t addr) {
  (void)gb;

  ESP_LOGE(TAG, "Emulator error=%d at addr=0x%04x", (int)err, addr);
  set_last_error("runtime core error");
}

bool paperboy_gb_init(const uint8_t *rom, size_t rom_size) {
  enum gb_init_error_e init_err;
  size_t save_size;
  char rom_name[17];

  s_ready = false;
  set_last_error("not initialized");

  if (rom == NULL || rom_size < 0x150) {
    set_last_error("rom buffer missing or too small");
    return false;
  }

  memset(&s_gb, 0, sizeof(s_gb));
  memset(s_lcd, 0, sizeof(s_lcd));
  memset(s_previous_lcd, 0, sizeof(s_previous_lcd));
  memset(s_wram, 0, sizeof(s_wram));
  memset(s_vram, 0, sizeof(s_vram));

  s_rom = (uint8_t *)rom;
  s_rom_size = rom_size;

  free(s_cart_ram);
  s_cart_ram = NULL;
  s_cart_ram_size = 0;

  init_err = gb_init(&s_gb, s_wram, s_vram, s_lcd, s_rom, s_rom_size, gb_error_cb, NULL,
                     s_cgb_preference);
  if (init_err != GB_INIT_NO_ERROR && init_err != GB_INIT_NO_ERROR_BUT_REQUIRES_CGB) {
    ESP_LOGE(TAG, "gb_init failed: %d", (int)init_err);
    set_last_error("gb_init failed");
    return false;
  }

  // audio_init() brings up the codec and its task, so it must only happen once:
  // a reset re-enters here without tearing audio down.
  static bool audio_started;

  if (!audio_started) {
    audio_init();
    audio_started = true;
  }
  gb_reset(&s_gb, s_cgb_preference);

  if (s_gb.is_cgb_mode) {
    // This core implements the CGB's hardware but not its colour: the palette
    // registers (0xFF68-0xFF6B) are ignored, and the picture is shaded by the
    // DMG palette registers instead. A colour game never writes those, so they
    // are seeded with the identity palette - colour index 0 renders as the
    // lightest shade and 3 as the darkest, which is the order CGB palettes are
    // normally built in, keeping the intended contrast of a colour scene.
    s_gb.gb_reg.BGP = 0xE4;
    s_gb.gb_reg.OBP0 = 0xE4;
    s_gb.gb_reg.OBP1 = 0xE4;
  }

  save_size = gb_get_save_size(&s_gb);
  if (save_size > 0) {
    s_cart_ram = calloc(1, save_size);
    if (s_cart_ram == NULL) {
      set_last_error("cart ram alloc failed");
      return false;
    }
    s_cart_ram_size = save_size;
    s_gb.gb_cart_ram = s_cart_ram;
    s_gb.gb_cart_ram_size = save_size;
  }

  gb_init_lcd(&s_gb);
  s_gb.direct.joypad = 0xFF;
  audio_enabled = 1;
  s_gb.direct.sram_updated = 0;
  s_gb.direct.sram_dirty = 0;

  ESP_LOGI(TAG, "core ready, ROM title: %s", gb_get_rom_name(s_rom, rom_name));

  s_ready = true;
  set_last_error("ok");
  return true;
}

void paperboy_gb_set_cgb_mode(bool enable) { s_cgb_preference = enable; }

// True when the machine actually came up in CGB mode: the ROM asks for it and
// the mode is enabled. A state load can change this mid-session.
bool paperboy_gb_is_cgb(void) { return s_ready && s_gb.is_cgb_mode; }

void paperboy_gb_set_buttons(uint8_t pressed_mask) {
  if (!s_ready) {
    return;
  }

  s_gb.direct.joypad = (uint8_t)(~pressed_mask);
}

// The buttons the core is currently being fed, for diagnostics: this is after
// the menu chord has been taken out, so it shows what the game itself sees.
uint8_t paperboy_gb_buttons(void) { return s_ready ? (uint8_t)~s_gb.direct.joypad : 0; }

bool paperboy_gb_run_frame(bool skip_render, int *dirty_g0, int *dirty_g1) {
  // The two interpreters are separate compilations of the same core, so the
  // runner follows the machine's mode rather than the session's preference:
  // restoring a state saved in the other mode switches it back.
  void (*run_frame)(gb_s *) = s_gb.is_cgb_mode ? gb_run_frame__cgb : gb_run_frame__dmg;
  bool any_dirty = false;
  int first_line = LCD_HEIGHT;
  int last_line = -1;

  if (dirty_g0 != NULL) {
    *dirty_g0 = 0;
  }
  if (dirty_g1 != NULL) {
    *dirty_g1 = -1;
  }

  if (!s_ready) {
    return false;
  }

  s_gb.direct.frame_skip = skip_render;
  run_frame(&s_gb);
  s_gb.direct.frame_skip = false;

  if (skip_render) {
    return true;
  }

  PROF_BEGIN(PROF_LCD);
  for (unsigned line = 0; line < LCD_HEIGHT; line++) {
    const size_t row_offset = line * LCD_WIDTH_PACKED;
    if (memcmp(&s_lcd[row_offset], &s_previous_lcd[row_offset], LCD_WIDTH_PACKED) == 0) {
      continue;
    }

    memcpy(&s_previous_lcd[row_offset], &s_lcd[row_offset], LCD_WIDTH_PACKED);
    blit_line(line);
    any_dirty = true;
    if ((int)line < first_line) {
      first_line = (int)line;
    }
    last_line = (int)line;
  }
  PROF_END(PROF_LCD);

  if (any_dirty && dirty_g0 != NULL && dirty_g1 != NULL) {
    // Scanline r occupies landscape rows 2r and 2r+1, which are panel columns
    // 299-2r and 298-2r, so the groups follow from the extremes.
    *dirty_g0 = (298 - 2 * last_line) / 12;
    *dirty_g1 = (299 - 2 * first_line) / 12;
  }

  return true;
}

// Forces the next frame to redraw every scanline, for when something else has
// drawn over the Game Boy area (menus, notices).
void paperboy_gb_invalidate_video(void) {
  if (s_ready) {
    gb_invalidate_video_cache();
  }
}

// One line of core registers, for the serial console.
void paperboy_gb_dump_state(char *out, size_t out_size) {
  if (!s_ready) {
    snprintf(out, out_size, "not ready");
    return;
  }

  snprintf(out, out_size,
           "pc=%04x sp=%04x af=%04x bc=%04x de=%04x hl=%04x ly=%02x lcdc=%02x stat=%02x "
           "ie=%02x if=%02x ime=%d halt=%d frame=%d ly=%02x bgmap=%04x",
           s_gb.cpu_reg.pc, s_gb.cpu_reg.sp, s_gb.cpu_reg.af, s_gb.cpu_reg.bc, s_gb.cpu_reg.de,
           s_gb.cpu_reg.hl, s_gb.gb_reg.LY, s_gb.gb_reg.LCDC, s_gb.gb_reg.STAT, s_gb.gb_reg.IE,
           s_gb.gb_reg.IF, (int)s_gb.gb_ime, (int)s_gb.gb_halt, (int)s_gb.gb_frame, s_gb.gb_reg.LYC,
           (unsigned)((uintptr_t)s_gb.display.bg_map_base - (uintptr_t)s_gb.vram));
}

bool paperboy_gb_has_persist(void) {
  if (!s_ready) {
    return false;
  }

  return gb_has_persist_data();
}

size_t paperboy_gb_persist_size(void) {
  if (!s_ready || !gb_has_persist_data()) {
    return 0;
  }

  return sizeof(struct persist_header) + s_cart_ram_size + gb_persist_rtc_size();
}

bool paperboy_gb_persist_is_dirty(void) {
  if (!s_ready || !gb_has_persist_data()) {
    return false;
  }

  return s_gb.direct.sram_updated != 0;
}

bool paperboy_gb_persist_export(uint8_t *dst, size_t dst_size, uint32_t timestamp) {
  struct persist_header header;
  uint8_t *cursor;

  if (!s_ready || !gb_has_persist_data() || dst == NULL) {
    set_last_error("persist export unavailable");
    return false;
  }

  if (dst_size < paperboy_gb_persist_size()) {
    set_last_error("persist export buffer too small");
    return false;
  }

  memcpy(header.magic, PERSIST_MAGIC, sizeof(header.magic));
  header.version = PERSIST_VERSION;
  header.has_cart_ram = (uint8_t)(s_cart_ram_size > 0);
  header.has_rtc = (uint8_t)(s_gb.cart_battery != 0);
  header.reserved = 0;
  header.cart_ram_size = (uint32_t)s_cart_ram_size;
  header.timestamp = timestamp;

  memcpy(dst, &header, sizeof(header));
  cursor = dst + sizeof(header);

  if (s_cart_ram_size > 0) {
    memcpy(cursor, s_cart_ram, s_cart_ram_size);
    cursor += s_cart_ram_size;
  }

  if (s_gb.cart_battery) {
    memcpy(cursor, s_gb.cart_rtc, sizeof(s_gb.cart_rtc));
  }

  set_last_error("ok");
  return true;
}

bool paperboy_gb_persist_import(const uint8_t *src, size_t src_size, uint32_t current_timestamp) {
  struct persist_header header;
  const uint8_t *cursor;

  if (!s_ready || !gb_has_persist_data() || src == NULL) {
    set_last_error("persist import unavailable");
    return false;
  }

  if (src_size < sizeof(header)) {
    set_last_error("persist blob too small");
    return false;
  }

  memcpy(&header, src, sizeof(header));
  if (memcmp(header.magic, PERSIST_MAGIC, sizeof(header.magic)) != 0 ||
      header.version != PERSIST_VERSION) {
    set_last_error("persist blob format mismatch");
    return false;
  }

  if ((header.has_cart_ram != 0) != (s_cart_ram_size > 0) ||
      header.cart_ram_size != s_cart_ram_size) {
    set_last_error("persist cart ram mismatch");
    return false;
  }

  if ((header.has_rtc != 0) != (s_gb.cart_battery != 0)) {
    set_last_error("persist rtc mismatch");
    return false;
  }

  if (src_size !=
      sizeof(header) + header.cart_ram_size + (header.has_rtc ? sizeof(s_gb.cart_rtc) : 0u)) {
    set_last_error("persist blob size mismatch");
    return false;
  }

  cursor = src + sizeof(header);
  if (header.cart_ram_size > 0) {
    memcpy(s_cart_ram, cursor, header.cart_ram_size);
    cursor += header.cart_ram_size;
  }

  if (header.has_rtc) {
    memcpy(s_gb.cart_rtc, cursor, sizeof(s_gb.cart_rtc));
    if (header.timestamp > 0 && current_timestamp >= header.timestamp) {
      gb_catch_up_rtc_direct(&s_gb, current_timestamp - header.timestamp);
    }
    memcpy(s_gb.latched_rtc, s_gb.cart_rtc, sizeof(s_gb.latched_rtc));
  }

  s_gb.direct.sram_updated = 0;
  s_gb.direct.sram_dirty = 0;
  set_last_error("ok");
  return true;
}

void paperboy_gb_persist_mark_clean(void) {
  if (!s_ready) {
    return;
  }

  s_gb.direct.sram_updated = 0;
  s_gb.direct.sram_dirty = 0;
}

size_t paperboy_gb_state_size(void) {
  if (!s_ready) {
    return 0;
  }

  return gb_get_state_size(&s_gb);
}

bool paperboy_gb_state_export(uint8_t *dst, size_t dst_size) {
  const size_t state_size = paperboy_gb_state_size();

  if (!s_ready || dst == NULL || dst_size < state_size) {
    set_last_error("state export buffer too small");
    return false;
  }

  gb_state_save(&s_gb, (char *)dst);
  set_last_error("ok");
  return true;
}

bool paperboy_gb_state_import(const uint8_t *src, size_t src_size) {
  const char *result;

  if (!s_ready || src == NULL) {
    set_last_error("state import unavailable");
    return false;
  }

  result = gb_state_load(&s_gb, (const char *)src, src_size);
  if (result != NULL) {
    set_last_error(result);
    return false;
  }

  // gb_state_load deliberately preserves the derived pointers rather than
  // restoring them, so the tile-map bases have to be rebuilt from the restored
  // LCDC. The core only does that when the game writes LCDC (and at init), so a
  // restored state would otherwise render from whatever map base the emulator
  // happened to have — which is wrong whenever that predates the state, such as
  // loading a save straight after boot.
  __gb_update_map_pointers(&s_gb);

  gb_invalidate_video_cache();
  set_last_error("ok");
  return true;
}

bool paperboy_gb_is_ready(void) { return s_ready; }

const char *paperboy_gb_last_error(void) { return s_last_error; }
