// gameboy_rlcd ROM manager: a separate application in its own partition.
//
// It exists because one image cannot hold both the emulator's buffers and WiFi:
// they want the same internal RAM, and everything tried in a single image ended
// up either failing to start the radio or moving the emulator's memory somewhere
// slower. The emulator lives in ota_0 and carries none of this; this app lives in
// ota_1 and carries none of the emulator. The SD card - and so the ROMs, the
// saves and the config file - is common to both.
//
// Entering this app and leaving it are both a call to esp_ota_set_boot_partition
// followed by a restart, which is the bootloader's business rather than ours.
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "buttons.h"
#include "config.h"
#include "fb.h"
#include "rom_files.h"
#include "st7305.h"
#include "storage_sd.h"
#include "web.h"

static const char *TAG = "manager";

// One line of the screen, at the same 12-pixel size the emulator's menus use.
// The row is filled first, so an inverted one reads as a bar rather than as text
// with gaps around it.
static void row(int index, const char *text, bool text_ink, bool bg_ink) {
  fb_fill_rect(0, index * 16, LCD_W, 16, bg_ink);
  fb_text(6, index * 16, text, 2, text_ink, bg_ink);
}

static void bytes_text(uint64_t bytes, char *out, size_t out_size) {
  static const char *const kUnits[] = {"B", "KB", "MB", "GB"};
  double value = (double)bytes;
  int unit = 0;

  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    unit++;
  }
  snprintf(out, out_size, (unit == 0) ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
}

// Back to the emulator. Reached by holding both buttons, and also the only way
// out of this app, so it has to work even if everything else failed.
static void switch_to_emulator(void) {
  const esp_partition_t *emulator =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);

  if (emulator == NULL || esp_ota_set_boot_partition(emulator) != ESP_OK) {
    ESP_LOGE(TAG, "cannot switch back to ota_0");
    return;
  }

  ESP_LOGI(TAG, "switching to %s", emulator->label);
  esp_restart();
}

void app_main(void) {
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  ESP_LOGI(TAG, "rom manager starting");

  if (!rlcd_init()) {
    ESP_LOGE(TAG, "panel init failed");
    return;
  }

  buttons_init();

  fb_clear(false);
  row(6, "Starting up", true, false);
  rlcd_flush_all(fb_buffer());

  const bool have_sd = storage_sd_mount();
  const bool serving = have_sd && web_start();

  if (!serving) {
    ESP_LOGE(TAG, "not serving: %s", have_sd ? "WiFi failed" : "no SD card");
  }

  int rom_count = 0;
  int tick = 0;

  while (true) {
    char summary[48];
    char free_text[24];

    if (serving) {
      uint64_t total = 0;
      uint64_t free_bytes = 0;
      rom_files_space(&total, &free_bytes);
      bytes_text(free_bytes, free_text, sizeof(free_text));
      snprintf(summary, sizeof(summary), "%d ROMs, %s free", rom_count, free_text);
    } else {
      snprintf(summary, sizeof(summary), "%s", have_sd ? "WiFi did not start" : "No SD card");
    }

    char clients[48];
    const int joined = web_client_count();
    if (joined == 0) {
      snprintf(clients, sizeof(clients), "Waiting for a device");
    } else {
      snprintf(clients, sizeof(clients), "%d device%s connected", joined,
               (joined == 1) ? "" : "s");
    }

    fb_clear(false);
    row(0, "ROM MANAGER", true, false);
    if (serving) {
      row(3, "Join WiFi", false, true);
      row(4, web_ssid(), false, true);
      row(6, "Password", false, true);
      row(7, web_password(), false, true);
      row(9, "Then open", false, true);
      row(10, web_ip(), false, true);
    }
    row(13, summary, true, false);
    row(14, clients, true, false);
    row(17, "hold BOTH:back to games", true, false);
    rlcd_flush_all(fb_buffer());

    // The card only changes when someone uses the page, so walking it every
    // tick would be wasted work.
    if (++tick >= 8 && serving) {
      tick = 0;
      const int found = rom_files_count();
      if (found != rom_count) {
        ESP_LOGI(TAG, "card holds %d ROM(s)", found);
        rom_count = found;
      }
    }

    if (buttons_poll() == (BTN_KEY | BTN_BOOT) && buttons_hold_ms() >= BTN_CHORD_MS) {
      switch_to_emulator();
    }

    vTaskDelay(pdMS_TO_TICKS(250));
  }
}
