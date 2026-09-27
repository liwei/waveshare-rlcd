// gameboy_rlcd: a Game Boy emulator for the Waveshare ESP32-S3-RLCD-4.2.
//
// The emulator core is PaperBoy's CrankBoy/Peanut-GB build; this file is the
// front end: storage, input, audio, the frame loop and the menus.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio.h"
#include "battery.h"
#include "buttons.h"
#include "config.h"
#include "console.h"
#include "fb.h"
#include "gbemu.h"
#include "input.h"
#include "input_bt.h"
#include "profiler.h"
#include "st7305.h"
#include "storage_sd.h"
#include "ui.h"

static const char *TAG = "gameboy_rlcd";

#define CFG_PATH SD_MOUNT_POINT "/gameboy.cfg"
#define SAVE_EXT ".sav"
// Save states live in numbered slots, one file each: <rom>.st1 and so on. Four
// is enough to keep a checkpoint without turning the card into a mess, and the
// extension is deliberately not ".state" so the old automatic snapshot cannot
// be mistaken for one of them.
#define STATE_SLOTS 4
#define STATE_SLOT_EXT_MAX 8

// Slot 0 is the quick one: R2 writes it and L2 reads it, without a menu.
#define STATE_QUICK_SLOT 0

static bool state_exists(const char *rom_path, int slot);
static int resume_slot(const char *rom_path);

typedef struct {
  char last_rom[256];
  int volume;
  int audio_engine;
  bool muted;
  bool cgb; /* run colour cartridges on the CGB core instead of as a DMG */
  pad_binding_t padmap[PAD_BINDING_MAX]; /* button mappings, keyed by pad model */
  int padmap_count;
  int last_slot; /* the save slot last written or read, for resuming */
} cfg_t;

static cfg_t s_cfg;
static char s_rom_path[256];
static uint8_t *s_rom_data;
static size_t s_rom_size;

/* ------------------------------------------------------------------ config */

// One <vid>:<pid>:<one index per Game Boy button> line per re-bound pad.
static bool cfg_parse_padmap(const char *value) {
  if (s_cfg.padmap_count >= PAD_BINDING_MAX) {
    return false;
  }

  char *end;
  const unsigned long vid = strtoul(value, &end, 16);
  if (*end != ':') {
    return false;
  }
  const unsigned long pid = strtoul(end + 1, &end, 16);
  if (*end != ':') {
    return false;
  }

  pad_binding_t *binding = &s_cfg.padmap[s_cfg.padmap_count];
  binding->vid = (uint16_t)vid;
  binding->pid = (uint16_t)pid;

  const char *cursor = end + 1;
  for (int action = 0; action < PAD_ACTION_COUNT; action++) {
    const long index = strtol(cursor, &end, 10);
    if (end == cursor || index < -1 || index > 31) {
      return false;
    }
    binding->index[action] = (int8_t)index;
    cursor = (*end == ',') ? end + 1 : end;
  }

  // Reject anything that carries more values than this model has. The set of
  // Game Boy buttons has changed once already, and silently reading the first
  // few values of a longer line produced a mapping that looked mangled rather
  // than absent - it is better to fall back to the built-in table.
  if (*end != '\0') {
    return false;
  }

  s_cfg.padmap_count++;
  return true;
}

static void cfg_load(void) {
  memset(&s_cfg, 0, sizeof(s_cfg));
  s_cfg.volume = AUDIO_VOLUME_DEFAULT;
  s_cfg.audio_engine = AUDIO_ENGINE_PCM;
  s_cfg.cgb = true;
  s_cfg.last_slot = 1;

  FILE *f = fopen(CFG_PATH, "r");
  if (f == NULL) {
    return;
  }

  char line[320];
  while (fgets(line, sizeof(line), f) != NULL) {
    line[strcspn(line, "\r\n")] = '\0';
    if (strncmp(line, "last_rom=", 9) == 0) {
      strlcpy(s_cfg.last_rom, line + 9, sizeof(s_cfg.last_rom));
    } else if (strncmp(line, "volume=", 7) == 0) {
      s_cfg.volume = atoi(line + 7);
    } else if (strncmp(line, "audio_engine=", 13) == 0) {
      s_cfg.audio_engine = atoi(line + 13);
    } else if (strncmp(line, "muted=", 6) == 0) {
      s_cfg.muted = atoi(line + 6) != 0;
    } else if (strncmp(line, "cgb=", 4) == 0) {
      s_cfg.cgb = atoi(line + 4) != 0;
    } else if (strncmp(line, "last_slot=", 10) == 0) {
      const int slot = atoi(line + 10);
      if (slot >= STATE_QUICK_SLOT && slot <= STATE_SLOTS) {
        s_cfg.last_slot = slot;
      }
    } else if (strncmp(line, "padmap=", 7) == 0) {
      if (!cfg_parse_padmap(line + 7)) {
        ESP_LOGW(TAG, "ignoring a padmap line this build cannot use: %s", line + 7);
      }
    }
  }
  fclose(f);
}

static void cfg_save(void) {
  FILE *f = fopen(CFG_PATH, "w");
  if (f == NULL) {
    return;
  }
  if (s_cfg.last_rom[0] != '\0') {
    fprintf(f, "last_rom=%s\n", s_cfg.last_rom);
  }
  fprintf(f, "volume=%d\n", s_cfg.volume);
  fprintf(f, "audio_engine=%d\n", s_cfg.audio_engine);
  fprintf(f, "muted=%d\n", s_cfg.muted ? 1 : 0);
  fprintf(f, "cgb=%d\n", s_cfg.cgb ? 1 : 0);
  fprintf(f, "last_slot=%d\n", s_cfg.last_slot);

  for (int i = 0; i < s_cfg.padmap_count; i++) {
    fprintf(f, "padmap=%04x:%04x:", s_cfg.padmap[i].vid, s_cfg.padmap[i].pid);
    for (int action = 0; action < PAD_ACTION_COUNT; action++) {
      fprintf(f, "%s%d", (action > 0) ? "," : "", s_cfg.padmap[i].index[action]);
    }
    fputc('\n', f);
  }
  fclose(f);
}

/* ------------------------------------------------------------------ saves */

// Replaces the ROM's extension with `ext`.
static bool sibling_path(const char *rom_path, const char *ext, char *out, size_t out_size) {
  const char *slash = strrchr(rom_path, '/');
  const char *dot = strrchr(rom_path, '.');
  if (dot == NULL || (slash != NULL && dot < slash)) {
    dot = rom_path + strlen(rom_path);
  }

  const size_t base = (size_t)(dot - rom_path);
  if (base + strlen(ext) + 1 > out_size) {
    return false;
  }

  memcpy(out, rom_path, base);
  memcpy(out + base, ext, strlen(ext) + 1);
  return true;
}

static bool save_persist(const char *rom_path) {
  if (!paperboy_gb_has_persist()) {
    return false;
  }

  char path[256];
  if (!sibling_path(rom_path, SAVE_EXT, path, sizeof(path))) {
    return false;
  }

  const size_t size = paperboy_gb_persist_size();
  uint8_t *blob = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (blob == NULL) {
    return false;
  }

  bool ok = false;
  const uint32_t timestamp = (uint32_t)(esp_timer_get_time() / 1000000);
  if (paperboy_gb_persist_export(blob, size, timestamp)) {
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
      ok = (fwrite(blob, 1, size, f) == size);
      fclose(f);
    }
  }

  heap_caps_free(blob);
  if (ok) {
    paperboy_gb_persist_mark_clean();
    ESP_LOGI(TAG, "saved %s (%u bytes)", path, (unsigned)size);
  }
  return ok;
}

static bool load_persist(const char *rom_path) {
  if (!paperboy_gb_has_persist()) {
    return false;
  }

  char path[256];
  if (!sibling_path(rom_path, SAVE_EXT, path, sizeof(path))) {
    return false;
  }

  size_t size = 0;
  uint8_t *blob = storage_sd_read_file(path, &size);
  if (blob == NULL) {
    return false;
  }

  const uint32_t timestamp = (uint32_t)(esp_timer_get_time() / 1000000);
  const bool ok = paperboy_gb_persist_import(blob, size, timestamp);
  storage_free(blob);

  if (ok) {
    ESP_LOGI(TAG, "loaded %s", path);
  }
  return ok;
}

// The slot's file, next to the ROM: "pokemon.gb" slot 2 -> "pokemon.st2".
static bool state_path(const char *rom_path, int slot, char *out, size_t out_size) {
  if (slot < STATE_QUICK_SLOT || slot > STATE_SLOTS) {
    return false;
  }

  char ext[STATE_SLOT_EXT_MAX];
  snprintf(ext, sizeof(ext), ".st%d", slot);
  return sibling_path(rom_path, ext, out, out_size);
}

static bool save_state(const char *rom_path, int slot) {
  char path[256];
  if (!state_path(rom_path, slot, path, sizeof(path))) {
    return false;
  }

  const size_t size = paperboy_gb_state_size();
  if (size == 0) {
    return false;
  }

  uint8_t *blob = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (blob == NULL) {
    return false;
  }

  bool ok = false;
  if (paperboy_gb_state_export(blob, size)) {
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
      ok = (fwrite(blob, 1, size, f) == size);
      fclose(f);
    }
  }

  heap_caps_free(blob);
  ESP_LOGI(TAG, "save state %s: %s", path, ok ? "ok" : "failed");
  return ok;
}

static bool load_state(const char *rom_path, int slot) {
  char path[256];
  if (!state_path(rom_path, slot, path, sizeof(path))) {
    return false;
  }

  size_t size = 0;
  uint8_t *blob = storage_sd_read_file(path, &size);
  if (blob == NULL) {
    return false;
  }

  const bool ok = paperboy_gb_state_import(blob, size);
  storage_free(blob);
  ESP_LOGI(TAG, "load state %s: %s", path, ok ? "ok" : "failed");
  return ok;
}

/* -------------------------------------------------------------------- ROMs */

static bool rom_load(const char *path) {
  size_t size = 0;
  uint8_t *data = storage_sd_read_file(path, &size);
  if (data == NULL) {
    return false;
  }

  free(s_rom_data);
  s_rom_data = data;
  s_rom_size = size;
  strlcpy(s_rom_path, path, sizeof(s_rom_path));

  ESP_LOGI(TAG, "loaded %s (%u bytes)", path, (unsigned)size);
  return true;
}

/* ------------------------------------------------------------------- menus */

static void menu_bluetooth(void);

static const char *bt_usage_name(const esp_hid_scan_result_t *result) {
  const char *usage = esp_hid_usage_str(result->usage);
  return (usage != NULL) ? usage : "HID";
}

// Scans and lets the user pick a pad. Returns the chosen result, or NULL.
static const esp_hid_scan_result_t *bt_choose_device(esp_hid_scan_result_t **results_out,
                                                     size_t *count_out) {
  *results_out = NULL;
  *count_out = 0;

  input_bt_scan_start(5);

  while (input_bt_scan_state() == INPUT_BT_SCAN_RUNNING) {
    ui_clear();
    ui_row(UI_TITLE_ROW, "BLUETOOTH", false, true);
    ui_row(6, "Scanning...", true, false);
    ui_row(8, "Put the pad in pairing mode", true, false);
    ui_status_bar();
    ui_flush();
    console_poll();
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  esp_hid_scan_result_t *results = NULL;
  const size_t count = input_bt_scan_results(&results);
  if (count == 0 || results == NULL) {
    ui_notice("BLUETOOTH", "No devices found", 1800);
    return NULL;
  }

  char(*names)[64] = heap_caps_malloc(sizeof(char[64]) * count, MALLOC_CAP_SPIRAM);
  const char **items = heap_caps_malloc(sizeof(char *) * count, MALLOC_CAP_SPIRAM);
  if (names == NULL || items == NULL) {
    heap_caps_free(names);
    heap_caps_free(items);
    return NULL;
  }

  size_t index = 0;
  for (const esp_hid_scan_result_t *r = results; r != NULL && index < count; r = r->next) {
    snprintf(names[index], sizeof(names[0]), "%s (%s)", (r->name != NULL) ? r->name : "unnamed",
             bt_usage_name(r));
    items[index] = names[index];
    index++;
  }

  const int chosen = ui_menu("SELECT GAMEPAD", items, (int)index, 0, NULL);

  const esp_hid_scan_result_t *picked = NULL;
  if (chosen >= 0) {
    size_t i = 0;
    for (const esp_hid_scan_result_t *r = results; r != NULL; r = r->next, i++) {
      if (i == (size_t)chosen) {
        picked = r;
        break;
      }
    }
  }

  heap_caps_free(items);
  heap_caps_free(names);

  *results_out = results;
  *count_out = count;
  return picked;
}

/* -------------------------------------------------- gamepad button mapping */

// The page lists the Game Boy's own buttons and asks which pad button plays
// each. Swapping two of them is a matter of assigning both by hand - there is
// no mode or profile to reason about.
#define MAPPING_RESET_ROW PAD_ACTION_COUNT

static const char *const kMappingItems[PAD_ACTION_COUNT + 1] = {
    "A",       "B",         "Start",     "Select",    "Up",       "Down",
    "Left",    "Right",     "Menu",      "Reset this pad",
};

// The stored binding for this pad, seeded from what the pad currently uses so
// every entry written out is complete.
static pad_binding_t *binding_for_pad(uint16_t vid, uint16_t pid) {
  for (int i = 0; i < s_cfg.padmap_count; i++) {
    if (s_cfg.padmap[i].vid == vid && s_cfg.padmap[i].pid == pid) {
      return &s_cfg.padmap[i];
    }
  }
  if (s_cfg.padmap_count >= PAD_BINDING_MAX) {
    return NULL;
  }

  pad_binding_t *binding = &s_cfg.padmap[s_cfg.padmap_count++];
  binding->vid = vid;
  binding->pid = pid;
  for (int action = 0; action < PAD_ACTION_COUNT; action++) {
    binding->index[action] = input_bt_action_get(action);
  }
  return binding;
}

static void forget_binding(uint16_t vid, uint16_t pid) {
  for (int i = 0; i < s_cfg.padmap_count; i++) {
    if (s_cfg.padmap[i].vid != vid || s_cfg.padmap[i].pid != pid) {
      continue;
    }
    for (int j = i + 1; j < s_cfg.padmap_count; j++) {
      s_cfg.padmap[j - 1] = s_cfg.padmap[j];
    }
    s_cfg.padmap_count--;
    return;
  }
}

// Another Game Boy button already sitting on this pad button, if any.
static const char *action_using(const pad_binding_t *binding, int action, int index) {
  for (int i = 0; i < PAD_ACTION_COUNT; i++) {
    if (i != action && binding->index[i] == index) {
      return kMappingItems[i];
    }
  }
  return NULL;
}

static void mapping_badge(int index, char *out, size_t out_size) {
  if (index >= PAD_ACTION_COUNT) {
    return;
  }

  const int8_t bound = input_bt_action_get(index);
  if (bound >= 0) {
    snprintf(out, out_size, "%.14s", input_bt_index_name(bound));
  } else if (index >= PAD_ACTION_UP && index <= PAD_ACTION_RIGHT && input_bt_has_hat()) {
    // Nothing to assign: this pad sends the D-pad as a hat, which the direction
    // handling picks up by itself.
    snprintf(out, out_size, "hat");
  } else {
    snprintf(out, out_size, "not set");
  }
}

#define CAPTURE_CANCELLED (-1)
#define CAPTURE_CLEARED (-2)

// Waits for a pad button to be pressed, and answers with its index - or one of
// the two codes above if the player clears the binding or gives up. The timeout
// only exists so a screen cannot trap the device; it is long enough to read the
// screen and try more than one button.
static int capture_pad_button(const char *label) {
  const int64_t deadline = esp_timer_get_time() + 60 * 1000000;

  input_bt_capture_begin();

  while (true) {
    ui_clear();
    ui_row(UI_TITLE_ROW, "KEY MAPPING", false, true);
    ui_row(3, label, false, true);
    ui_row(6, "Press the pad button to use", true, false);

    // Show what the pad is saying, so a pad that is not being heard looks
    // different from one whose button is simply not the one expected.
    const uint32_t live = input_bt_live_buttons();
    char seen[32];
    if (live != 0) {
      snprintf(seen, sizeof(seen), "Pad: %.24s", input_bt_index_name(__builtin_ctz(live)));
    } else {
      snprintf(seen, sizeof(seen), "Pad: -");
    }
    ui_row(9, seen, true, false);

    ui_row(UI_HINT_ROW, "KEY:clear hold KEY+BOOT:cancel", true, false);
    ui_status_bar();
    ui_flush();
    console_poll();

    const int captured = input_bt_capture_take();
    if (captured >= 0) {
      input_bt_capture_end();
      return captured;
    }

    // Clearing is the destructive choice, so it waits out the chord grace
    // period: a cancel that is a few milliseconds out of step must not wipe the
    // assignment on its way past.
    const uint8_t raw = buttons_poll();
    const uint32_t held = buttons_hold_ms();
    if (raw == (BTN_KEY | BTN_BOOT)) {
      if (held >= BTN_CHORD_MS) {
        input_bt_capture_end();
        return CAPTURE_CANCELLED;
      }
    } else if ((raw & BTN_KEY) && held >= BTN_CHORD_GRACE_MS) {
      input_bt_capture_end();
      return CAPTURE_CLEARED;
    }

    // Never leave the device on a screen with no way out.
    if (esp_timer_get_time() > deadline) {
      input_bt_capture_end();
      return CAPTURE_CANCELLED;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void menu_key_mapping(void) {
  uint16_t vid;
  uint16_t pid;

  if (!input_bt_connected()) {
    ui_notice("KEY MAPPING", "Connect a gamepad first", 1500);
    return;
  }
  if (!input_bt_identity(&vid, &pid)) {
    ui_notice("KEY MAPPING", "Gamepad not ready", 1500);
    return;
  }

  // The flow is two steps and neither the list nor its hint row says so: a row
  // has to be opened before a pad button means anything.
  ui_notice("KEY MAPPING", "Pick a button, then press the pad", 1800);

  int selection = 0;
  while (true) {
    const int chosen = ui_menu("KEY MAPPING", kMappingItems, PAD_ACTION_COUNT + 1, selection,
                               mapping_badge);
    if (chosen < 0) {
      return;
    }
    selection = chosen;

    if (chosen == MAPPING_RESET_ROW) {
      forget_binding(vid, pid);
      input_bt_binding_reset(vid, pid);
      cfg_save();
      ui_notice("KEY MAPPING", "Back to defaults", 900);
      continue;
    }

    pad_binding_t *binding = binding_for_pad(vid, pid);
    if (binding == NULL) {
      ui_notice("KEY MAPPING", "No room for another pad", 1500);
      continue;
    }

    const int captured = capture_pad_button(kMappingItems[chosen]);
    if (captured == CAPTURE_CANCELLED) {
      continue;
    }

    char message[48];
    if (captured == CAPTURE_CLEARED) {
      binding->index[chosen] = -1;
      snprintf(message, sizeof(message), "%.24s cleared", kMappingItems[chosen]);
    } else {
      binding->index[chosen] = (int8_t)captured;
      const char *clash = action_using(binding, chosen, captured);
      if (clash != NULL) {
        snprintf(message, sizeof(message), "%.12s also on %.12s", kMappingItems[chosen], clash);
      } else {
        snprintf(message, sizeof(message), "%.12s is now %.16s", kMappingItems[chosen],
                 input_bt_index_name(captured));
      }
    }

    input_bt_binding_set(binding);
    cfg_save();
    ui_notice("KEY MAPPING", message, 900);
  }
}

// Hand over to the ROM manager. It is a separate application in a separate
// partition - one image cannot have both the emulator's buffers and WiFi's - so
// switching is the bootloader's job: point it at the other slot and restart.
static void switch_to_rom_manager(void) {
  const esp_partition_t *manager =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
  if (manager == NULL) {
    ui_notice("ROM MANAGER", "Not installed", 2000);
    return;
  }

  ui_notice("ROM MANAGER", "Switching over...", 1200);
  if (esp_ota_set_boot_partition(manager) != ESP_OK) {
    ui_notice("ROM MANAGER", "Could not switch", 2000);
    return;
  }

  ESP_LOGI(TAG, "switching to %s", manager->label);
  esp_restart();
}

static void menu_bluetooth(void) {
  static const char *const items[] = {
      "Scan for gamepads",
      "Forget gamepad",
      "Button mapping",
  };
  int selection = 0;

  while (true) {
    esp_hid_scan_result_t *results = NULL;
    size_t count = 0;

    const int chosen =
        ui_menu("BLUETOOTH", items, (int)(sizeof(items) / sizeof(items[0])), selection, NULL);
    if (chosen < 0) {
      return;
    }
    selection = chosen;

    if (chosen == 1) {
      input_bt_forget();
      ui_notice("BLUETOOTH", "Forgotten", 900);
      continue;
    }

    if (chosen == 2) {
      menu_key_mapping();
      continue;
    }

    const esp_hid_scan_result_t *picked = bt_choose_device(&results, &count);
    if (picked != NULL) {
      input_bt_connect(picked);
      ui_notice("BLUETOOTH", "Connecting...", 1200);

      for (int i = 0; i < 40 && !input_bt_connected(); i++) {
        console_poll();
        vTaskDelay(pdMS_TO_TICKS(100));
      }

      ui_notice("BLUETOOTH", input_bt_connected() ? input_bt_name() : "No connection", 1200);
    }
  }
}

// What the pause menu asked for.
typedef enum {
  RUN_RESUME = 0, /* keep playing */
  RUN_RESET,      /* restart the current game */
  RUN_EXIT,       /* back to the ROM picker */
} run_action_t;

// Right-hand status for the pause menu rows, so the current level is visible
// without having to open anything.
static void pause_badge(int index, char *out, size_t out_size) {
  switch (index) {
    case 2:
      if (s_cfg.last_slot == STATE_QUICK_SLOT) {
        snprintf(out, out_size, "quick");
      } else {
        snprintf(out, out_size, "slot %d", s_cfg.last_slot);
      }
      break;
    case 3:
      snprintf(out, out_size, "%s", audio_is_muted() ? "muted" : "on");
      break;
    case 4:
    case 5:
      snprintf(out, out_size, "%d%%", s_cfg.volume);
      break;
    case 6:
      // The machine's mode, not the preference: a colour cartridge runs as a
      // DMG until the CGB core is selected.
      snprintf(out, out_size, "%s", paperboy_gb_is_cgb() ? "CGB" : "DMG");
      break;
    case 7:
      snprintf(out, out_size, "%s", input_bt_connected() ? "linked" : "off");
      break;
    default:
      break;
  }
}

// The four slots, marked with what is in them, for saving into or loading from.
// Returns the chosen slot number, or 0 if nothing was picked.
static void slot_badge(int index, char *out, size_t out_size) {
  snprintf(out, out_size, "%s", state_exists(s_rom_path, index + 1) ? "used" : "empty");
}

static int menu_state_slot(const char *title, bool saving) {
  static const char *const kSlots[STATE_SLOTS] = {"Slot 1", "Slot 2", "Slot 3", "Slot 4"};
  int selection = 0;

  while (true) {
    const int chosen = ui_menu(title, kSlots, STATE_SLOTS, selection, slot_badge);
    if (chosen < 0) {
      return 0;
    }
    selection = chosen;

    const int slot = chosen + 1;
    if (saving && state_exists(s_rom_path, slot)) {
      // Writing over a checkpoint is worth one question.
      static const char *const kConfirm[] = {"Overwrite it", "Keep it"};
      char question[32];
      snprintf(question, sizeof(question), "SLOT %d IS USED", slot);
      if (ui_menu(question, kConfirm, 2, 1, NULL) != 0) {
        continue;
      }
    }
    return slot;
  }
}

static run_action_t menu_pause(void) {
  static const char *const items[] = {
      "Resume",
      "Save state",
      "Load state",
      "Sound: toggle",
      "Volume down",
      "Volume up",
      "Video mode",
      "Bluetooth",
      "Reset game",
      "Change game",
  };

  // Stay on the entry that was just used, so repeating an action (nudging the
  // volume, say) does not mean walking back down the list each time.
  int selection = 0;

  while (true) {
    const int chosen =
        ui_menu("PAUSED", items, (int)(sizeof(items) / sizeof(items[0])), selection, pause_badge);
    if (chosen < 0 || chosen == 0) {
      return RUN_RESUME;
    }
    selection = chosen;

    switch (chosen) {
      case 1: {
        const int slot = menu_state_slot("SAVE STATE", true);
        if (slot != 0) {
          const bool ok = save_state(s_rom_path, slot);
          if (ok) {
            s_cfg.last_slot = slot;
            cfg_save();
          }
          ui_notice("SAVE", ok ? "Saved" : "Could not write", 700);
        }
        break;
      }
      case 2: {
        const int slot = menu_state_slot("LOAD STATE", false);
        if (slot != 0) {
          const bool ok = load_state(s_rom_path, slot);
          if (ok) {
            s_cfg.last_slot = slot;
            cfg_save();
          }
          ui_notice("LOAD", ok ? "Loaded" : "That slot is empty", 700);
        }
        break;
      }
      case 3: {
        const bool muted = !audio_is_muted();
        audio_set_muted(muted);
        s_cfg.muted = muted;
        cfg_save();
        ui_notice("SOUND", muted ? "Muted" : "On", 600);
        break;
      }
      case 4:
        s_cfg.volume = (s_cfg.volume > 5) ? s_cfg.volume - 5 : 0;
        audio_set_volume(s_cfg.volume);
        cfg_save();
        break;
      case 5:
        s_cfg.volume = (s_cfg.volume < 95) ? s_cfg.volume + 5 : 100;
        audio_set_volume(s_cfg.volume);
        cfg_save();
        break;
      case 6:
        // Takes effect on the next init, which is what RUN_RESET does.
        s_cfg.cgb = !s_cfg.cgb;
        cfg_save();
        ui_notice("VIDEO", s_cfg.cgb ? "CGB" : "DMG", 600);
        return RUN_RESET;
      case 7:
        menu_bluetooth();
        break;
      case 8:
        return RUN_RESET;
      case 9:
        return RUN_EXIT;
      default:
        break;
    }
  }
}

/* --------------------------------------------------------------------- loop */

static TaskHandle_t s_frame_task;
static esp_timer_handle_t s_frame_timer;

// Fires once per emulated frame; the loop blocks on the notification so the
// idle task still runs instead of being starved by a spin wait.
static void frame_tick(void *arg) {
  (void)arg;
  xTaskNotifyGive(s_frame_task);
}

// The Game Boy picture only covers x 40..360, so anything the menus drew in the
// margins or over the picture has to be cleared before play resumes.
static void redraw_screen(void) {
  fb_clear(false);
  ui_status_bar();
  paperboy_gb_invalidate_video();
  rlcd_flush_all(fb_buffer());
}

// The status strip is the bottom 12 framebuffer rows, which is exactly panel
// column group 0, so it can be refreshed on its own. Redraw it and report
// whether anything actually changed, so the panel is only touched when the
// battery or Bluetooth text really moves.
static uint8_t s_status_shadow[STATUS_H * LCD_STRIDE];

static bool refresh_status_bar(void) {
  const uint8_t *strip = fb_row(STATUS_Y);
  if (strip == NULL) {
    return false;
  }

  memcpy(s_status_shadow, strip, sizeof(s_status_shadow));
  ui_status_bar();
  return memcmp(s_status_shadow, strip, sizeof(s_status_shadow)) != 0;
}

// Whether to leave this frame unpainted. A CGB frame can cost more than the
// 16.7 ms a 60 Hz panel allows, and the panel flush has to fit in the same
// budget. Every frame is still emulated - only the painting is dropped - so the
// game keeps its correct speed and the audio keeps its cadence; the picture
// just advances at 30 Hz for as long as the core is behind.
static int64_t s_frame_ema_us;

static bool frame_should_skip(void) {
  static uint32_t index;
  static bool skipping;

  const uint32_t n = index++;
  const int mode = console_frame_skip_mode();

  if (mode == FRAME_SKIP_NEVER) {
    skipping = false;
    return false;
  }
  if (mode == FRAME_SKIP_ALTERNATE) {
    skipping = true;
    return (n & 1u) != 0;
  }

  // Auto, with hysteresis so it does not flap between the two rates.
  if (!skipping && s_frame_ema_us > (GB_FRAME_US * 9) / 10) {
    skipping = true;
  } else if (skipping && s_frame_ema_us < (GB_FRAME_US * 3) / 5) {
    skipping = false;
  }

  return skipping && (n & 1u) != 0;
}

static void frame_note_emu_time(int64_t us) { s_frame_ema_us = (s_frame_ema_us * 7 + us) / 8; }

// Runs frames until the player leaves the game, and says why they left.
static run_action_t run_emulator(void) {
  run_action_t action = RUN_EXIT;

  s_frame_task = xTaskGetCurrentTaskHandle();

  const esp_timer_create_args_t timer_args = {
      .callback = frame_tick,
      .name = "gbframe",
  };
  if (esp_timer_create(&timer_args, &s_frame_timer) == ESP_OK) {
    esp_timer_start_periodic(s_frame_timer, GB_FRAME_US);
  }

  redraw_screen();

  int64_t next_status_us = 0;

  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    const int64_t t_start = esp_timer_get_time();
    const uint8_t pad = input_read();
    paperboy_gb_set_buttons(pad);

    // R2 and L2: a save and a load with no menu in the way, for trying
    // something and putting it back if it fails. Saved states are per ROM and
    // survive the session, so this is the same slot every time until it is
    // overwritten.
    if (input_bt_take_quick_save()) {
      const bool ok = save_state(s_rom_path, STATE_QUICK_SLOT);
      if (ok) {
        s_cfg.last_slot = STATE_QUICK_SLOT;
        cfg_save();
      }
      ui_toast(ok ? "SAVED" : "SAVE FAILED", 2500);
      refresh_status_bar();
      rlcd_flush_groups(fb_buffer(), 0, 0);
    }
    if (input_bt_take_quick_load()) {
      const bool ok = load_state(s_rom_path, STATE_QUICK_SLOT);
      if (ok) {
        s_cfg.last_slot = STATE_QUICK_SLOT;
        cfg_save();
      }
      ui_toast(ok ? "LOADED" : "NOTHING SAVED", 2500);
      refresh_status_bar();
      rlcd_flush_groups(fb_buffer(), 0, 0);
    }

    if (input_pause_requested()) {
      paperboy_gb_set_buttons(0);
      action = menu_pause();
      if (action != RUN_RESUME) {
        break;
      }
      redraw_screen();
      audio_service_frame();
      continue;
    }

    PROF_BEGIN(PROF_FRAME);
    int g0 = 0;
    int g1 = -1;
    const bool ran = paperboy_gb_run_frame(frame_should_skip(), &g0, &g1);
    PROF_END(PROF_FRAME);

    if (!ran) {
      ESP_LOGE(TAG, "run_frame failed: %s", paperboy_gb_last_error());
      break;
    }
    const int64_t t_emu = esp_timer_get_time();
    frame_note_emu_time(t_emu - t_start);

    const int64_t now = t_emu;
    if (now >= next_status_us) {
      next_status_us = now + 1000000;
      if (refresh_status_bar()) {
        rlcd_flush_groups(fb_buffer(), 0, 0);
      }
    }
    if (g1 >= g0) {
      rlcd_flush_groups(fb_buffer(), g0, g1);
    }
    const int64_t t_flush = esp_timer_get_time();

    const int64_t t_audio_start = esp_timer_get_time();
    audio_service_frame();
    const int64_t t_audio = esp_timer_get_time();
    profiler_frame_end(false);
    console_frame(t_flush - t_start, t_emu - t_start, t_flush - t_emu, t_audio - t_audio_start,
                  g1 >= g0 ? g0 : 0, g1);
    console_poll();
  }

  if (s_frame_timer != NULL) {
    esp_timer_stop(s_frame_timer);
    esp_timer_delete(s_frame_timer);
    s_frame_timer = NULL;
  }

  return action;
}

// Runs a ROM image already in memory. Loops on a reset request and returns when
// the player asks to change game.
static void run_rom(const uint8_t *rom, size_t size, const char *path, bool want_snapshot) {
  run_action_t action = RUN_RESET;

  while (action == RUN_RESET) {
    audio_set_engine((audio_engine_t)s_cfg.audio_engine);
    paperboy_gb_set_cgb_mode(s_cfg.cgb);
    if (!paperboy_gb_init(rom, size)) {
      ui_notice("CORE", paperboy_gb_last_error(), 2000);
      return;
    }

    audio_set_volume(s_cfg.volume);
    audio_set_muted(s_cfg.muted);

    if (path != NULL) {
      load_persist(path);
      if (want_snapshot) {
        const int slot = resume_slot(path);
        if (slot != 0) {
          load_state(path, slot);
        }
      }
    }
    want_snapshot = false; /* only on the first pass */

    action = run_emulator();

    if (path != NULL) {
      // The cartridge's battery RAM is written on the way out, always: that is
      // the game's own save, and losing it would lose real progress. Snapshots
      // are not written automatically - they belong in the slot the player
      // chose, and silently overwriting one on exit is how a checkpoint
      // disappears.
      save_persist(path);
    }
  }
}

// Whether the previous session left a snapshot for this ROM.
static bool state_exists(const char *rom_path, int slot) {
  char path[256];
  struct stat st;

  if (!state_path(rom_path, slot, path, sizeof(path))) {
    return false;
  }
  return stat(path, &st) == 0;
}

// The slot a resume should use: the one most recently saved or loaded, if it is
// still there, otherwise the first that is.
static int resume_slot(const char *rom_path) {
  if (s_cfg.last_slot >= STATE_QUICK_SLOT && s_cfg.last_slot <= STATE_SLOTS &&
      state_exists(rom_path, s_cfg.last_slot)) {
    return s_cfg.last_slot;
  }

  for (int slot = 1; slot <= STATE_SLOTS; slot++) {
    if (state_exists(rom_path, slot)) {
      return slot;
    }
  }
  return 0;
}

// Offer to pick up where the last session left off. "New game" boots the cart
// itself, so the game's own in-game save (which is a separate file) is
// untouched and it can still offer its own continue.
static bool ask_resume(const char *rom_path) {
  static const char *const items[] = {"Resume", "New game"};
  const char *name = strrchr(rom_path, '/');
  char title[48];

  name = (name != NULL) ? name + 1 : rom_path;
  snprintf(title, sizeof(title), "%.40s", name);

  return ui_menu(title, items, 2, 0, NULL) == 0;
}

void app_main(void) {
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  ESP_LOGI(TAG, "gameboy_rlcd starting");
  if (!rlcd_init()) {
    ESP_LOGE(TAG, "panel init failed");
    return;
  }

  ui_clear();
  ui_notice("GAMEBOY RLCD", "Starting up", 0);

  battery_init();
  input_init();
  console_init();
  input_bt_init();

  const bool have_sd = storage_sd_mount();
  if (!have_sd) {
    ui_notice("SD CARD", "Not found - built-in ROM", 2500);
  }

  cfg_load();

  // Hand the saved button mappings over before any pad can connect, so a pad
  // that is already bonded comes up with the mapping its owner set.
  for (int i = 0; i < s_cfg.padmap_count; i++) {
    input_bt_binding_set(&s_cfg.padmap[i]);
  }

  while (true) {
    char rom_path[256] = {0};
    bool want_snapshot = false;

    const ui_rom_pick_result_t pick = ui_rom_picker(
        have_sd ? SD_MOUNT_POINT : NULL, rom_path, sizeof(rom_path), s_cfg.last_rom);
    if (pick == UI_ROM_PICK_NONE) {
      continue;
    }

    if (pick == UI_ROM_PICK_MANAGER) {
      switch_to_rom_manager();
      continue;
    }

    if (pick == UI_ROM_PICK_LOAD_LAST) {
      strlcpy(rom_path, s_cfg.last_rom, sizeof(rom_path));
      want_snapshot = true;
    } else if (resume_slot(rom_path) != 0) {
      want_snapshot = ask_resume(rom_path);
    }

    ui_notice("LOADING", rom_path + strlen(SD_MOUNT_POINT "/"), 0);
    if (!rom_load(rom_path)) {
      ui_notice("LOADING", "Could not read ROM", 1500);
      continue;
    }

    // A colour-only cartridge has to run on the CGB core; as a DMG it draws
    // garbage rather than reporting an error, so offer the switch instead of
    // pretending to run it.
    if ((s_rom_data[0x143] & 0xC0u) == 0xC0u && !s_cfg.cgb) {
      static const char *const items[] = {"Switch to CGB", "Back"};
      if (ui_menu("NEEDS GAME BOY COLOR", items, 2, 0, NULL) != 0) {
        continue;
      }
      s_cfg.cgb = true;
      cfg_save();
    }

    strlcpy(s_cfg.last_rom, rom_path, sizeof(s_cfg.last_rom));
    cfg_save();
    run_rom(s_rom_data, s_rom_size, rom_path, want_snapshot);
  }
}
