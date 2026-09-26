#include "ui.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery.h"
#include "config.h"
#include "console.h"
#include "fb.h"
#include "gbemu.h"
#include "input.h"
#include "st7305.h"

static const char *TAG = "ui";

#define UI_MAX_ROMS 96
#define UI_PATH_MAX 192
#define UI_TEXT_SCALE 2

typedef char ui_path_t[UI_PATH_MAX];

static ui_path_t *s_roms;
static int s_rom_count;
static uint8_t *s_rom_cgb; /* 0 = DMG, 0x80 = CGB-enhanced, 0xC0 = CGB-only */
static int s_picker_fixed;

// The cartridge header's CGB flag (byte 0x143): 0x80 means the game is
// colour-enhanced but still runs on a DMG, 0xC0 means it needs a Game Boy
// Color. This core is DMG-only, so 0xC0 titles have to be refused rather than
// rendered into garbage.
static uint8_t rom_cgb_flag(const char *path) {
  FILE *f = fopen(path, "rb");
  uint8_t value = 0;

  if (f == NULL) {
    return 0;
  }
  if (fseek(f, 0x143, SEEK_SET) == 0) {
    if (fread(&value, 1, 1, f) != 1) {
      value = 0;
    }
  }
  fclose(f);
  return (uint8_t)(value & 0xC0u);
}

// Right-hand marker so CGB-only entries can be spotted before selecting them.
static void picker_badge(int index, char *out, size_t out_size) {
  out[0] = '\0';

  const int rom = index - s_picker_fixed;
  if (rom < 0 || rom >= s_rom_count || s_rom_cgb == NULL) {
    return;
  }

  if (s_rom_cgb[rom] == 0xC0u) {
    snprintf(out, out_size, "CGB only");
  } else if (s_rom_cgb[rom] == 0x80u) {
    snprintf(out, out_size, "CGB");
  }
}

void ui_clear(void) { fb_clear(false); }

void ui_flush(void) { rlcd_flush_all(fb_buffer()); }

void ui_text(int x, int y, const char *s, int scale, bool text_ink, bool bg_ink) {
  fb_text(x, y, s, scale, text_ink, bg_ink);
}

void ui_row(int row, const char *text, bool text_ink, bool bg_ink) {
  const int y = row * UI_ROW_H;
  fb_fill_rect(0, y, LCD_W, UI_ROW_H, bg_ink);
  if (text != NULL && *text != '\0') {
    fb_text(6, y + (UI_ROW_H - FB_CHAR_H * UI_TEXT_SCALE) / 2, text, UI_TEXT_SCALE, text_ink,
            bg_ink);
  }
}

void ui_status_bar(void) {
  const int y = STATUS_Y;
  const int text_y = y + (STATUS_H - FB_CHAR_H) / 2;

  fb_fill_rect(0, y, LCD_W, STATUS_H, false);

  char left[48];
  if (input_bt_connected()) {
    snprintf(left, sizeof(left), "BT: %s", input_bt_name());
  } else {
    snprintf(left, sizeof(left), "BT: --");
  }
  fb_text(4, text_y, left, 1, true, false);

  char right[16];
  if (battery_valid()) {
    snprintf(right, sizeof(right), "%d%%", battery_percent());
  } else {
    snprintf(right, sizeof(right), "n/a");
  }
  fb_battery(LCD_W - 22, y + 2, battery_percent(), battery_charging());
  fb_text(LCD_W - 22 - 4 - fb_text_width(right, 1), text_y, right, 1, true, false);
}

void ui_notice(const char *title, const char *message, uint32_t duration_ms) {
  ui_clear();
  ui_row(UI_TITLE_ROW, title, false, true);
  ui_row(6, message, true, false);
  ui_status_bar();
  ui_flush();

  if (duration_ms > 0) {
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
  }
}

// True while a menu owns the screen, so the console can report it.
static volatile bool s_menu_active;

bool ui_menu_active(void) { return s_menu_active; }

int ui_menu(const char *title, const char *const *items, int count, int initial, ui_badge_fn badge) {
  int selection = (initial >= 0 && initial < count) ? initial : 0;
  int scroll = 0;
  bool redraw = true;

  s_menu_active = true;

  // A menu opened by a held button (the pause chord) must not treat that
  // button as a fresh press.
  input_menu_reset();

  if (selection >= UI_LIST_VISIBLE) {
    scroll = selection - UI_LIST_VISIBLE + 1;
  }

  while (true) {
    if (redraw) {
      redraw = false;
      ui_clear();
      ui_row(UI_TITLE_ROW, title, false, true);

      if (scroll > 0) {
        char ind[32];
        snprintf(ind, sizeof(ind), "^ %d above", scroll);
        ui_row(UI_SCROLL_UP_ROW, ind, true, false);
      }

      for (int i = 0; i < UI_LIST_VISIBLE; i++) {
        const int index = scroll + i;
        const int row = UI_LIST_FIRST + i;

        if (index >= count) {
          continue;
        }

        const bool sel = (index == selection);
        ui_row(row, items[index], sel ? false : true, sel ? true : false);

        if (badge != NULL) {
          char text[32];
          text[0] = '\0';
          badge(index, text, sizeof(text));
          if (text[0] != '\0') {
            const int x = LCD_W - 6 - fb_text_width(text, UI_TEXT_SCALE);
            fb_text(x, row * UI_ROW_H + (UI_ROW_H - FB_CHAR_H * UI_TEXT_SCALE) / 2, text,
                    UI_TEXT_SCALE, sel ? false : true, sel ? true : false);
          }
        }
      }

      if (count - scroll - UI_LIST_VISIBLE > 0) {
        char ind[32];
        snprintf(ind, sizeof(ind), "v %d below", count - scroll - UI_LIST_VISIBLE);
        ui_row(UI_SCROLL_DOWN_ROW, ind, true, false);
      }

      // Names the buttons for the hold gesture: "hold:back" left people guessing
      // which of the two to hold, and holding the wrong one steps or activates.
      ui_row(UI_HINT_ROW, "KEY:next BOOT:go hold BOTH:back", true, false);
      ui_status_bar();
      ui_flush();
    }

    // Menus are the only thing running here, so the console has to be pumped
    // from this loop too.
    console_poll();

    const uint8_t ev = input_menu_events();

    // The two physical buttons can only step one way, so the cursor wraps:
    // overshooting an entry is always recoverable by going round again.
    if (ev & GB_BTN_DOWN) {
      selection = (selection + 1) % count;
      if (selection == 0) {
        scroll = 0;
      } else if (selection >= scroll + UI_LIST_VISIBLE) {
        scroll = selection - UI_LIST_VISIBLE + 1;
      }
      redraw = true;
    }

    if (ev & GB_BTN_UP) {
      selection = (selection + count - 1) % count;
      if (selection == count - 1) {
        scroll = (count > UI_LIST_VISIBLE) ? count - UI_LIST_VISIBLE : 0;
      } else if (selection < scroll) {
        scroll = selection;
      }
      redraw = true;
    }

    if (ev & (GB_BTN_A | GB_BTN_START)) {
      s_menu_active = false;
      return selection;
    }

    if (ev & (GB_BTN_B | GB_BTN_SELECT)) {
      s_menu_active = false;
      return -1;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static bool has_rom_extension(const char *path) {
  const char *dot = strrchr(path, '.');
  if (dot == NULL) {
    return false;
  }
  return strcasecmp(dot, ".gb") == 0 || strcasecmp(dot, ".gbc") == 0;
}

static int scan_dir(const char *dir_path, ui_path_t *files, int max, int count, int depth) {
  DIR *dir = opendir(dir_path);
  if (dir == NULL) {
    return count;
  }

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL && count < max) {
    if (ent->d_name[0] == '.') {
      continue;
    }

    char full[512];
    snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);

    uint8_t dtype = ent->d_type;
    if (dtype == DT_UNKNOWN) {
      struct stat st;
      if (stat(full, &st) == 0) {
        dtype = S_ISDIR(st.st_mode) ? DT_DIR : DT_REG;
      } else {
        dtype = DT_REG;
      }
    }

    if (dtype == DT_DIR) {
      if (depth > 0) {
        count = scan_dir(full, files, max, count, depth - 1);
      }
    } else if (has_rom_extension(ent->d_name)) {
      strlcpy(files[count], full, UI_PATH_MAX - 1);
      files[count][UI_PATH_MAX - 1] = '\0';
      if (s_rom_cgb != NULL) {
        s_rom_cgb[count] = rom_cgb_flag(files[count]);
      }
      // Logged as bytes: non-ASCII names arrive UTF-8 encoded, and this is how
      // to tell a filesystem encoding problem from a font one.
      ESP_LOGI(TAG, "rom %d: %s (cgb flag 0x%02x)", count, files[count],
               s_rom_cgb != NULL ? s_rom_cgb[count] : 0);
      count++;
    }
  }

  closedir(dir);
  return count;
}
int ui_rom_count(void) { return s_rom_count; }

const char *ui_rom_name(int index) {
  if (index < 0 || index >= s_rom_count) {
    return NULL;
  }
  const char *slash = strrchr(s_roms[index], '/');
  return (slash != NULL) ? slash + 1 : s_roms[index];
}

ui_rom_pick_result_t ui_rom_picker(const char *mount_pt, char *out_path, size_t path_size,
                                   const char *last_rom) {
  if (s_roms == NULL) {
    s_roms = (ui_path_t *)heap_caps_malloc(UI_MAX_ROMS * sizeof(ui_path_t), MALLOC_CAP_SPIRAM);
    s_rom_cgb = (uint8_t *)heap_caps_malloc(UI_MAX_ROMS, MALLOC_CAP_SPIRAM);
    if (s_roms == NULL || s_rom_cgb == NULL) {
      ESP_LOGE(TAG, "no memory for the ROM list");
      return UI_ROM_PICK_NONE;
    }
  }

  s_rom_count = (mount_pt != NULL) ? scan_dir(mount_pt, s_roms, UI_MAX_ROMS, 0, 1) : 0;
  ESP_LOGI(TAG, "found %d ROM(s) under %s", s_rom_count, mount_pt != NULL ? mount_pt : "(none)");

  const int has_last = (last_rom != NULL && last_rom[0] != '\0') ? 1 : 0;
  const int fixed = 1 + has_last; /* built-in, then optional last played */
  const int count = s_rom_count + fixed;
  s_picker_fixed = fixed;

  const char **items =
      (const char **)heap_caps_malloc(sizeof(char *) * (size_t)count, MALLOC_CAP_SPIRAM);
  if (items == NULL) {
    return UI_ROM_PICK_NONE;
  }

  items[0] = "Built-in test ROM";
  if (has_last) {
    items[1] = "Last played";
  }
  for (int i = 0; i < s_rom_count; i++) {
    items[i + fixed] = ui_rom_name(i);
  }

  const int chosen = ui_menu("SELECT GAME", items, count, 0, picker_badge);
  heap_caps_free(items);

  if (chosen < 0) {
    return UI_ROM_PICK_NONE;
  }
  if (chosen == 0) {
    return UI_ROM_PICK_BUILTIN;
  }
  if (has_last && chosen == 1) {
    return UI_ROM_PICK_LOAD_LAST;
  }

  snprintf(out_path, path_size, "%s", s_roms[chosen - fixed]);
  return UI_ROM_PICK_SELECTED;
}
