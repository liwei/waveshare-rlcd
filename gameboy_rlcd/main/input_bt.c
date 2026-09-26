// Bluetooth gamepad: BLE HID host built on esp_hidh.
//
// The ESP32-S3 has no BR/EDR, so only BLE HID (HOGP) pads are supported. The
// pad's report descriptor is parsed at connect time, which makes any pad that
// exposes a standard gamepad collection work without a per-model table.
#include <stdbool.h>
#include <stddef.h>
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
static bool s_spend_chord;

// Learning a button: the next one pressed becomes the answer, and until that
// happens the pad drives nothing at all.
static volatile bool s_capture;
static volatile bool s_capture_resync;
static volatile int s_capture_result;
static uint32_t s_capture_prev;
static volatile bool s_swallow_buttons;
static volatile bool s_has_hat; /* the pad reports its D-pad as a hat switch */
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

// The button each Game Boy button is played with on this pad: the built-in
// table for the model, with any user binding laid over the top. Everything that
// reads the mapping reads this, so a binding takes effect without its existence
// being known about anywhere else.
static int8_t s_actions[PAD_ACTION_COUNT];
static pad_binding_t s_bindings[PAD_BINDING_MAX];
static int s_binding_count;

// The built-in button for an action. This is where the deliberate A/B exchange
// lives: the pad's A and Y, being the two buttons nearest the right thumb, play
// the Game Boy's B, and the pad's B and X play its A. Put another way, the
// mapping is set up to suit the hands rather than the labels, and the mapping
// page is where anyone who disagrees can say so.
static int8_t builtin_action(const pad_profile_t *p, int action) {
  switch (action) {
    case PAD_ACTION_A:
      return p->b;
    case PAD_ACTION_B:
      return p->a;
    case PAD_ACTION_START:
      return p->start;
    case PAD_ACTION_SELECT:
      return p->select;
    case PAD_ACTION_UP:
      return p->dpad_up;
    case PAD_ACTION_DOWN:
      return p->dpad_down;
    case PAD_ACTION_LEFT:
      return p->dpad_left;
    case PAD_ACTION_RIGHT:
      return p->dpad_right;
    case PAD_ACTION_MENU:
      return p->guide;
    default:
      return -1;
  }
}

static const pad_binding_t *binding_for(uint16_t vid, uint16_t pid) {
  for (int i = 0; i < s_binding_count; i++) {
    if (s_bindings[i].vid == vid && s_bindings[i].pid == pid) {
      return &s_bindings[i];
    }
  }
  return NULL;
}

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

// Fill in the button each Game Boy button plays with: the built-in table for
// this model, then any binding on top of it.
static void set_effective_profile(uint16_t vid, uint16_t pid) {
  s_profile = profile_for(vid, pid);

  for (int action = 0; action < PAD_ACTION_COUNT; action++) {
    s_actions[action] = builtin_action(s_profile, action);
  }

  const pad_binding_t *binding = binding_for(vid, pid);
  if (binding != NULL) {
    for (int action = 0; action < PAD_ACTION_COUNT; action++) {
      s_actions[action] = binding->index[action];
    }
  }
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

// Are either of the buttons playing Start or Select still held? Follows the
// mapping, so re-binding them moves the menu chord with them.
static bool pad_chord_buttons_down(const hid_gamepad_state_t *state) {
  const uint32_t b = state->buttons;
  const int8_t start = s_actions[PAD_ACTION_START];
  const int8_t select = s_actions[PAD_ACTION_SELECT];

  return (start >= 0 && ((b >> start) & 1u)) || (select >= 0 && ((b >> select) & 1u));
}

static uint8_t map_state_to_gb(const hid_gamepad_map_t *map, const hid_gamepad_state_t *state) {
  const pad_profile_t *p = s_profile;
  const uint32_t b = state->buttons;
  uint8_t mask = 0;

#define PRESSED(idx) ((idx) >= 0 && ((b >> (idx)) & 1u))
#define BOUND(action) (s_actions[action] >= 0)

  if (PRESSED(s_actions[PAD_ACTION_A])) {
    mask |= GB_BTN_A;
  }
  if (PRESSED(s_actions[PAD_ACTION_B])) {
    mask |= GB_BTN_B;
  }
  if (PRESSED(s_actions[PAD_ACTION_START])) {
    mask |= GB_BTN_START;
  }
  if (PRESSED(s_actions[PAD_ACTION_SELECT])) {
    mask |= GB_BTN_SELECT;
  }

  // Directions a binding owns outright. Everything else falls through to the
  // pad's automatic handling below, so a pad that reports its D-pad as a hat
  // keeps working without any of this being configured, and picking a button
  // for one direction does not silence the other three.
  const uint8_t bound_directions =
      (uint8_t)((BOUND(PAD_ACTION_UP) ? GB_BTN_UP : 0) | (BOUND(PAD_ACTION_DOWN) ? GB_BTN_DOWN : 0) |
                (BOUND(PAD_ACTION_LEFT) ? GB_BTN_LEFT : 0) |
                (BOUND(PAD_ACTION_RIGHT) ? GB_BTN_RIGHT : 0));

  if (PRESSED(s_actions[PAD_ACTION_UP])) {
    mask |= GB_BTN_UP;
  }
  if (PRESSED(s_actions[PAD_ACTION_DOWN])) {
    mask |= GB_BTN_DOWN;
  }
  if (PRESSED(s_actions[PAD_ACTION_LEFT])) {
    mask |= GB_BTN_LEFT;
  }
  if (PRESSED(s_actions[PAD_ACTION_RIGHT])) {
    mask |= GB_BTN_RIGHT;
  }

  uint8_t automatic = 0;

  if (state->hat >= 0) {
    switch (state->hat) {
      case 0:
        automatic |= GB_BTN_UP;
        break;
      case 1:
        automatic |= GB_BTN_UP | GB_BTN_RIGHT;
        break;
      case 2:
        automatic |= GB_BTN_RIGHT;
        break;
      case 3:
        automatic |= GB_BTN_DOWN | GB_BTN_RIGHT;
        break;
      case 4:
        automatic |= GB_BTN_DOWN;
        break;
      case 5:
        automatic |= GB_BTN_DOWN | GB_BTN_LEFT;
        break;
      case 6:
        automatic |= GB_BTN_LEFT;
        break;
      case 7:
        automatic |= GB_BTN_UP | GB_BTN_LEFT;
        break;
      default:
        break;
    }
  } else {
    automatic |= stick_to_dpad(map, state);
  }

  mask |= (uint8_t)(automatic & ~bound_directions);

  // The pad's own menu gesture: the Menu button when one is mapped, or
  // Start+Select together, which every pad here has. Edge-detected so holding
  // the buttons opens the menu once rather than re-opening it on every report.
  const bool menu_now =
      PRESSED(s_actions[PAD_ACTION_MENU]) ||
      (PRESSED(s_actions[PAD_ACTION_START]) && PRESSED(s_actions[PAD_ACTION_SELECT]));

#undef BOUND
#undef PRESSED

  if (menu_now && !s_menu_prev) {
    s_menu_edge = true;
    // The chord is spent on the menu: its buttons stay swallowed until they are
    // let go, so Start+Select does not carry on into whatever happens next.
    s_spend_chord = true;
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
      set_effective_profile(vid, pid);
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

      // Whether the D-pad arrives as a hat, which decides what a direction that
      // nobody has bound a button to falls back to.
      s_has_hat = false;
      for (int i = 0; i < s_map_count; i++) {
        if (s_maps[i].has_hat) {
          s_has_hat = true;
          break;
        }
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

      if (s_capture) {
        // The first report only sets the baseline, so a button already held when
        // the screen opened is not mistaken for the one being pressed now.
        if (s_capture_resync) {
          s_capture_prev = state.buttons;
          s_capture_resync = false;
        } else {
          const uint32_t fresh = state.buttons & ~s_capture_prev;
          s_capture_prev = state.buttons;
          if (fresh != 0 && s_capture_result < 0) {
            s_capture_result = __builtin_ctz(fresh);
          }
        }
        s_gb_mask = 0;
        break;
      }

      s_gb_mask = map_state_to_gb(map, &state);

      // After a binding, the player is still holding the button they just used.
      // Nothing reaches the menu until they let go: a held d-pad would otherwise
      // scroll the page out from under them.
      if (s_swallow_buttons) {
        s_gb_mask = 0;
        if (state.buttons == 0) {
          s_swallow_buttons = false;
        }
        break;
      }

      // Start+Select opened the pause menu, so that press belongs to the menu
      // and not to the game: without this the buttons reach the game again as
      // soon as play resumes - and most titles read a held Start as "open the
      // map". Both are swallowed until they are released, after which the pad
      // behaves normally again (Start and Select are ordinary menu buttons).
      if (s_spend_chord) {
        s_gb_mask &= (uint8_t)~(GB_BTN_START | GB_BTN_SELECT);
        if (!pad_chord_buttons_down(&state)) {
          s_spend_chord = false;
        }
      }
      break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
      ESP_LOGI(TAG, "disconnected (%s)", esp_hid_disconnect_reason_str(ESP_HID_TRANSPORT_BLE,
                                                                     param->close.reason));
      s_connected = false;
      s_gb_mask = 0;
      s_menu_edge = false;
      s_spend_chord = false;
      s_capture = false;
      s_swallow_buttons = false;
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

int8_t input_bt_action_get(int action) {
  if (action < 0 || action >= PAD_ACTION_COUNT) {
    return -1;
  }
  return s_actions[action];
}

const char *input_bt_index_name(int index) {
  static char unnamed[24];
  const pad_profile_t *p = s_profile;

  if (index < 0) {
    return "nothing";
  }

  // Which of the pad's named buttons sits on this index, per the profile.
  const struct {
    int8_t index;
    const char *name;
  } names[] = {
      {p->a, "pad A"},     {p->b, "pad B"},         {p->x, "pad X"},      {p->y, "pad Y"},
      {p->lb, "LB"},       {p->rb, "RB"},           {p->start, "Start"},  {p->select, "Select"},
      {p->guide, "Home"},  {p->dpad_up, "d-pad up"}, {p->dpad_down, "d-pad down"},
      {p->dpad_left, "d-pad left"}, {p->dpad_right, "d-pad right"},
  };

  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (names[i].index >= 0 && names[i].index == index) {
      return names[i].name;
    }
  }

  snprintf(unnamed, sizeof(unnamed), "button %d", index);
  return unnamed;
}

bool input_bt_has_hat(void) { return s_has_hat; }

void input_bt_binding_set(const pad_binding_t *binding) {
  if (binding == NULL) {
    return;
  }

  pad_binding_t *slot = NULL;
  for (int i = 0; i < s_binding_count; i++) {
    if (s_bindings[i].vid == binding->vid && s_bindings[i].pid == binding->pid) {
      slot = &s_bindings[i];
      break;
    }
  }
  if (slot == NULL) {
    if (s_binding_count >= PAD_BINDING_MAX) {
      return;
    }
    slot = &s_bindings[s_binding_count++];
  }

  *slot = *binding;

  // If this is the pad being played with, the change takes effect now.
  uint16_t vid;
  uint16_t pid;
  if (input_bt_identity(&vid, &pid) && vid == binding->vid && pid == binding->pid) {
    set_effective_profile(vid, pid);
  }
}

void input_bt_binding_reset(uint16_t vid, uint16_t pid) {
  for (int i = 0; i < s_binding_count; i++) {
    if (s_bindings[i].vid != vid || s_bindings[i].pid != pid) {
      continue;
    }
    for (int j = i + 1; j < s_binding_count; j++) {
      s_bindings[j - 1] = s_bindings[j];
    }
    s_binding_count--;
    break;
  }

  uint16_t connected_vid;
  uint16_t connected_pid;
  if (input_bt_identity(&connected_vid, &connected_pid) && connected_vid == vid &&
      connected_pid == pid) {
    set_effective_profile(vid, pid);
  }
}

bool input_bt_identity(uint16_t *vid, uint16_t *pid) {
  if (!s_connected || s_dev == NULL) {
    return false;
  }

  if (vid != NULL) {
    *vid = esp_hidh_dev_vendor_id_get(s_dev);
  }
  if (pid != NULL) {
    *pid = esp_hidh_dev_product_id_get(s_dev);
  }
  return true;
}

void input_bt_capture_begin(void) {
  s_capture_result = -1;
  s_capture_resync = true;
  s_capture = true;
}

int input_bt_capture_take(void) {
  const int result = s_capture_result;
  if (result >= 0) {
    s_capture_result = -1;
  }
  return result;
}

void input_bt_capture_end(void) {
  s_capture = false;
  s_gb_mask = 0;
  s_swallow_buttons = true;
}

bool input_bt_take_menu(void) {
  const bool edge = s_menu_edge;
  s_menu_edge = false;
  return edge;
}
