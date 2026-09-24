#include "storage_sd.h"

#include <stdio.h>
#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "config.h"

static const char *TAG = "storage";

static sdmmc_card_t *s_card;
static bool s_ready;
static const char *s_error = "not mounted";

bool storage_sd_mount(void) {
  const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
      .format_if_mount_failed = false,
      .max_files = 8,
      .allocation_unit_size = 16 * 1024,
  };

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_1;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = SD_CLK_PIN;
  slot.cmd = SD_CMD_PIN;
  slot.d0 = SD_D0_PIN;
  // The board has no external pull-ups on the card lines.
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  const esp_err_t err =
      esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
  if (err != ESP_OK) {
    s_error = esp_err_to_name(err);
    ESP_LOGW(TAG, "TF card mount failed: %s", s_error);
    return false;
  }

  s_ready = true;
  s_error = "ok";
  ESP_LOGI(TAG, "TF card mounted at %s (%llu MB)", SD_MOUNT_POINT,
           (unsigned long long)s_card->csd.capacity / 2048);
  return true;
}

bool storage_sd_ready(void) { return s_ready; }

const char *storage_sd_error(void) { return s_error; }

uint8_t *storage_sd_read_file(const char *path, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    ESP_LOGW(TAG, "Cannot open %s", path);
    return NULL;
  }

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }

  const long len = ftell(f);
  if (len <= 0) {
    fclose(f);
    ESP_LOGW(TAG, "Empty or unseekable file: %s", path);
    return NULL;
  }

  rewind(f);

  uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (buf == NULL) {
    buf = (uint8_t *)heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
    if (buf != NULL) {
      ESP_LOGI(TAG, "%ld byte file in PSRAM", len);
    }
  }

  if (buf == NULL) {
    fclose(f);
    ESP_LOGE(TAG, "No memory for a %ld byte file", len);
    return NULL;
  }

  const size_t got = fread(buf, 1, (size_t)len, f);
  fclose(f);

  if (got != (size_t)len) {
    ESP_LOGE(TAG, "Short read on %s: %u/%ld", path, (unsigned)got, len);
    storage_free(buf);
    return NULL;
  }

  *out_size = (size_t)len;
  return buf;
}

void storage_free(void *p) { heap_caps_free(p); }
