// Bluetooth gamepad: BLE HID host built on esp_hidh.
//
// The ESP32-S3 has no BR/EDR, so only BLE HID (HOGP) pads are supported. The
// pad's report descriptor is parsed at connect time, which makes any pad that
// exposes a standard gamepad collection work without a per-model table.
#include <stdbool.h>
#include <string.h>

#include "esp_event.h"
#include "esp_hidh.h"
#include "esp_hidh_gattc.h"
#include "esp_hid_gap.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "config.h"
#include "gbemu.h"
#include "hid_gamepad.h"
#include "input.h"
#include "input_bt.h"

static const char *TAG = "btpad";

#define NVS_NAMESPACE "gbpad"
#define NVS_KEY_BDA "bda"
#define NVS_KEY_TYPE "type"

static volatile uint8_t s_gb_mask;
static volatile bool s_menu_edge;
static bool s_menu_prev;
static volatile bool s_connected;
static char s_name[48] = "";

static esp_hidh_dev_t *s_dev;
static hid_gamepad_map_t s_maps[HID_GP_MAX_REPORTS];
static int s_map_count;
static uint8_t s_bda[6];
static uint8_t s_addr_type;

static volatile int s_scan_state; /* 0 idle, 1 running, 2 done, 3 failed */
static esp_hid_scan_result_t *s_scan_results;
static size_t s_scan_count;
static uint32_t s_scan_seconds;
static volatile bool s_verbose; /* log every input report change */

/* ------------------------------------------------------------------- NVS */

static void bond_save(const uint8_t *bda, uint8_t addr_type) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    return;
  }
  nvs_set_blob(handle, NVS_KEY_BDA, bda, 6);
  nvs_set_u8(handle, NVS_KEY_TYPE, addr_type);
  nvs_commit(handle);
  nvs_close(handle);
}

static bool bond_load(uint8_t *bda, uint8_t *addr_type) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }
  size_t len = 6;
  const bool ok = (nvs_get_blob(handle, NVS_KEY_BDA, bda, &len) == ESP_OK) && len == 6;
  if (ok) {
    nvs_get_u8(handle, NVS_KEY_TYPE, addr_type);
  }
  nvs_close(handle);
  return ok;
}

static void bond_clear(void) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    return;
  }
  nvs_erase_key(handle, NVS_KEY_BDA);
  nvs_erase_key(handle, NVS_KEY_TYPE);
  nvs_commit(handle);
  nvs_close(handle);
}

/* ---------------------------------------------------------------- mapping */

// Gamepad buttons are numbered arbitrarily by each device, so the mapping has
// to be per-model. Profiles are matched on the USB-IF ids the pad reports;
// anything unmatched falls back to the Xbox layout. -1 means "no such button".
typedef struct {
  uint16_t vid;
  uint16_t pid;
  const char *name;
  int8_t a, b, x, y;
  int8_t lb, rb;
  int8_t select, start, guide;
  int8_t dpad_up, dpad_down, dpad_left, dpad_right; /* -1: d-pad is a hat */
} pad_profile_t;

static const pad_profile_t kProfiles[] = {
    // The ShanWan Q36 in Android mode identifies as an Amazon pad. Its layout
    // was read off the wire: A=0 B=1 X=3 Y=4 LB=6 RB=7 Start=10 Select=11. The
    // remaining indices were never observed, so no guide button is claimed;
    // Start+Select opens the menu instead.
    {0x1949, 0x0402, "Fire TV / Android", 0, 1, 3, 4, 6, 7, 11, 10, -1, -1, -1, -1, -1},
    // Xbox Wireless Controller and anything that copies it.
    {0x045E, 0x02FD, "Xbox", 0, 1, 2, 3, 4, 5, 6, 7, 8, 11, 12, 13, 14},
};

static const pad_profile_t kDefaultProfile = {
    0, 0, "generic (Xbox layout)", 0, 1, 2, 3, 4, 5, 6, 7, 8, 11, 12, 13, 14};

static const pad_profile_t *s_profile = &kDefaultProfile;

static const pad_profile_t *profile_for(uint16_t vid, uint16_t pid) {
  for (size_t i = 0; i < sizeof(kProfiles) / sizeof(kProfiles[0]); i++) {
    if (kProfiles[i].vid == vid && kProfiles[i].pid == pid) {
      return &kProfiles[i];
    }
  }
  return &kDefaultProfile;
}

static int32_t axis_centre(const hid_gamepad_map_t *map) {
  return (map->axis_min + map->axis_max) / 2;
}

static int32_t axis_deadzone(const hid_gamepad_map_t *map) {
  const int32_t span = (map->axis_max - map->axis_min) / 2;
  return span / 3;
}

// Pads without a hat switch or d-pad buttons steer with the left stick. HID Y
// grows downwards, which already matches the Game Boy's D-pad.
static uint8_t stick_to_dpad(const hid_gamepad_map_t *map, const hid_gamepad_state_t *state) {
  if (map == NULL || !map->has_x || !map->has_y) {
    return 0;
  }

  const int32_t deadzone = axis_deadzone(map);
  const int32_t centre = axis_centre(map);
  const int32_t x = state->x - centre;
  const int32_t y = state->y - centre;
  uint8_t mask = 0;

  if (y > deadzone) {
    mask |= GB_BTN_DOWN;
  } else if (y < -deadzone) {
    mask |= GB_BTN_UP;
  }

  if (x > deadzone) {
    mask |= GB_BTN_RIGHT;
  } else if (x < -deadzone) {
    mask |= GB_BTN_LEFT;
  }

  return mask;
}

static uint8_t map_state_to_gb(const hid_gamepad_map_t *map, const hid_gamepad_state_t *state) {
  const pad_profile_t *p = s_profile;
  const uint32_t b = state->buttons;
  uint8_t mask = 0;

#define PRESSED(idx) ((idx) >= 0 && ((b >> (idx)) & 1u))

  if (PRESSED(p->a) || PRESSED(p->y)) {
    mask |= GB_BTN_A;
  }
  if (PRESSED(p->b) || PRESSED(p->x)) {
    mask |= GB_BTN_B;
  }
  if (PRESSED(p->lb)) {
    mask |= GB_BTN_B;
  }
  if (PRESSED(p->rb)) {
    mask |= GB_BTN_A;
  }
  if (PRESSED(p->select)) {
    mask |= GB_BTN_SELECT;
  }
  if (PRESSED(p->start)) {
    mask |= GB_BTN_START;
  }

  if (state->hat >= 0) {
    switch (state->hat) {
      case 0:
        mask |= GB_BTN_UP;
        break;
      case 1:
        mask |= GB_BTN_UP | GB_BTN_RIGHT;
        break;
      case 2:
        mask |= GB_BTN_RIGHT;
        break;
      case 3:
        mask |= GB_BTN_DOWN | GB_BTN_RIGHT;
        break;
      case 4:
        mask |= GB_BTN_DOWN;
        break;
      case 5:
        mask |= GB_BTN_DOWN | GB_BTN_LEFT;
        break;
      case 6:
        mask |= GB_BTN_LEFT;
        break;
      case 7:
        mask |= GB_BTN_UP | GB_BTN_LEFT;
        break;
      default:
        break;
    }
  } else if (p->dpad_up >= 0) {
    if (PRESSED(p->dpad_up)) {
      mask |= GB_BTN_UP;
    }
    if (PRESSED(p->dpad_down)) {
      mask |= GB_BTN_DOWN;
    }
    if (PRESSED(p->dpad_left)) {
      mask |= GB_BTN_LEFT;
    }
    if (PRESSED(p->dpad_right)) {
      mask |= GB_BTN_RIGHT;
    }
  } else {
    mask |= stick_to_dpad(map, state);
  }

  // The pad's own menu gesture: a guide button when the profile knows one, or
  // Start+Select together, which every pad here has. Edge-detected so holding
  // the buttons opens the menu once rather than re-opening it on every report.
  const bool menu_now = PRESSED(p->guide) || (PRESSED(p->start) && PRESSED(p->select));

#undef PRESSED

  if (menu_now && !s_menu_prev) {
    s_menu_edge = true;
  }
  s_menu_prev = menu_now;

  return mask;
}

/* --------------------------------------------------------------- esp_hidh */

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
  esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;
  (void)handler_args;
  (void)base;

  switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT: {
      if (param->open.status != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(param->open.status));
        break;
      }

      s_dev = param->open.dev;
      const char *name = esp_hidh_dev_name_get(s_dev);
      strlcpy(s_name, (name != NULL) ? name : "gamepad", sizeof(s_name));

      const uint16_t vid = esp_hidh_dev_vendor_id_get(s_dev);
      const uint16_t pid = esp_hidh_dev_product_id_get(s_dev);
      s_profile = profile_for(vid, pid);
      ESP_LOGI(TAG, "pad %04x:%04x -> %s button map", vid, pid, s_profile->name);

      const uint8_t *bda = esp_hidh_dev_bda_get(s_dev);
      if (bda != NULL) {
        memcpy(s_bda, bda, 6);
        bond_save(s_bda, s_addr_type);
      }

      s_map_count = 0;
      size_t num_maps = 0;
      esp_hid_raw_report_map_t *report_maps = NULL;
      if (esp_hidh_dev_report_maps_get(s_dev, &num_maps, &report_maps) == ESP_OK) {
        for (size_t i = 0; i < num_maps && s_map_count < HID_GP_MAX_REPORTS; i++) {
          if (s_verbose) {
            char hex[3 * 128 + 1];
            size_t n = 0;
            for (size_t b = 0; b < report_maps[i].len && b < 128; b++) {
              n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%02x", report_maps[i].data[b]);
            }
            ESP_LOGI(TAG, "descriptor %u (%u bytes): %s", (unsigned)i,
                     (unsigned)report_maps[i].len, hex);
          }
          s_map_count += hid_gamepad_parse(report_maps[i].data, report_maps[i].len,
                                           &s_maps[s_map_count], HID_GP_MAX_REPORTS - s_map_count);
        }
      }

      if (s_map_count == 0) {
        ESP_LOGW(TAG, "no gamepad reports in the descriptor");
      }

      s_connected = true;
      ESP_LOGI(TAG, "connected to %s (%d report map(s))", s_name, s_map_count);
      break;
    }

    case ESP_HIDH_INPUT_EVENT: {
      if (param->input.data == NULL || param->input.length == 0) {
        break;
      }

      if (s_verbose) {
        // Log only when the report actually changes, so a held button does not
        // flood the console. Used to work out an unknown pad's button order.
        static uint8_t last[32];
        static size_t last_len;
        if (param->input.length != last_len ||
            memcmp(last, param->input.data, param->input.length) != 0) {
          last_len = param->input.length;
          memcpy(last, param->input.data, param->input.length);

          char hex[3 * sizeof(last) + 1];
          size_t n = 0;
          for (size_t i = 0; i < param->input.length && i < sizeof(last); i++) {
            n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%02x", param->input.data[i]);
          }
          ESP_LOGI(TAG, "report id=%u len=%u %s", (unsigned)param->input.report_id,
                   (unsigned)param->input.length, hex);
        }
      }

      const hid_gamepad_map_t *map =
          hid_gamepad_find(s_maps, s_map_count, (uint16_t)param->input.report_id);
      hid_gamepad_state_t state;
      hid_gamepad_decode(map, param->input.data, param->input.length, &state);
      s_gb_mask = map_state_to_gb(map, &state);
      break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
      ESP_LOGI(TAG, "disconnected (%s)", esp_hid_disconnect_reason_str(ESP_HID_TRANSPORT_BLE,
                                                                     param->close.reason));
      s_connected = false;
      s_gb_mask = 0;
      s_menu_edge = false;
      s_name[0] = '\0';
      s_dev = NULL;
      esp_hidh_dev_free(param->close.dev);
      break;
    }

    default:
      break;
  }
}

/* ------------------------------------------------------------------- scan */

static void scan_task(void *arg) {
  (void)arg;

  if (s_scan_results != NULL) {
    esp_hid_scan_results_free(s_scan_results);
    s_scan_results = NULL;
  }
  s_scan_count = 0;

  esp_hid_scan_result_t *results = NULL;
  size_t count = 0;
  const esp_err_t err = esp_hid_scan(s_scan_seconds, &count, &results);

  s_scan_results = results;
  s_scan_count = count;
  s_scan_state = (err == ESP_OK) ? 2 : 3;

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "scan found %u device(s)", (unsigned)count);
  } else {
    ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
  }

  vTaskDelete(NULL);
}

/* -------------------------------------------------------------------- API */

bool input_bt_ready(void) { return true; }

void input_bt_init(void) {
  esp_err_t err = esp_hid_gap_init(HIDH_BLE_MODE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gap init failed: %s", esp_err_to_name(err));
    return;
  }

  const esp_hidh_config_t config = {
      .callback = hidh_callback,
      .event_stack_size = 4096,
      .callback_arg = NULL,
  };
  // esp_hidh_init blocks until the GATTC registration completes, and that
  // completion only reaches it through this callback, so it has to be
  // registered first or the call never returns.
  err = esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gattc callback registration failed: %s", esp_err_to_name(err));
    return;
  }

  err = esp_hidh_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "hidh init failed: %s", esp_err_to_name(err));
    return;
  }

  uint8_t bda[6];
  uint8_t addr_type = 0;
  if (bond_load(bda, &addr_type)) {
    s_addr_type = addr_type;
    memcpy(s_bda, bda, sizeof(bda));
    ESP_LOGI(TAG, "reconnecting to the last gamepad");
    esp_hidh_dev_open(s_bda, ESP_HID_TRANSPORT_BLE, s_addr_type);
  }

  ESP_LOGI(TAG, "BLE HID host ready");
}

void input_bt_scan_start(uint32_t seconds) {
  if (s_scan_state == 1) {
    return;
  }
  s_scan_seconds = seconds;
  s_scan_state = 1;

  if (xTaskCreate(scan_task, "btscan", 4096, NULL, 4, NULL) != pdPASS) {
    s_scan_state = 3;
  }
}

int input_bt_scan_state(void) { return s_scan_state; }

size_t input_bt_scan_results(esp_hid_scan_result_t **out) {
  if (s_scan_state != 2 || out == NULL) {
    return 0;
  }
  *out = s_scan_results;
  return s_scan_count;
}

void input_bt_connect(const esp_hid_scan_result_t *result) {
  if (result == NULL) {
    return;
  }

  s_addr_type = (uint8_t)result->ble.addr_type;
  memcpy(s_bda, result->bda, sizeof(s_bda));

  ESP_LOGI(TAG, "connecting to %s", (result->name != NULL) ? result->name : "(unnamed)");
  esp_hidh_dev_open(s_bda, ESP_HID_TRANSPORT_BLE, s_addr_type);
}

void input_bt_forget(void) {
  if (s_dev != NULL) {
    esp_hidh_dev_close(s_dev);
  }
  s_connected = false;
  s_gb_mask = 0;
  s_name[0] = '\0';
  bond_clear();
  ESP_LOGI(TAG, "bond forgotten");
}

const char *input_bt_name(void) { return s_name; }

void input_bt_set_verbose(bool on) {
  s_verbose = on;
  ESP_LOGI(TAG, "report logging %s", on ? "on" : "off");
}

const char *input_bt_status_text(void) {
  switch (s_scan_state) {
    case INPUT_BT_SCAN_RUNNING:
      return "scanning";
    case INPUT_BT_SCAN_DONE:
      return "list ready";
    case INPUT_BT_SCAN_FAILED:
      return "scan failed";
    default:
      return s_connected ? s_name : "not connected";
  }
}

/* ------------------------------------------------------------------ input */

uint8_t input_bt_buttons(void) { return s_connected ? s_gb_mask : 0; }

bool input_bt_connected(void) { return s_connected; }

bool input_bt_take_menu(void) {
  const bool edge = s_menu_edge;
  s_menu_edge = false;
  return edge;
}
