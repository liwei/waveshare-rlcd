// Minimal HID report-descriptor parser for gamepads.
//
// esp_hid only classifies a device at the collection level, so this walks the
// raw report descriptor to find where the buttons, the hat switch and the X/Y
// axes live inside each input report.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_GP_MAX_REPORTS 4
// Pads commonly expose 32 buttons; the decoded mask is 32 bits wide.
#define HID_GP_MAX_BUTTONS 32

typedef struct {
  uint8_t report_id;
  bool valid;

  uint8_t button_count;
  int16_t button_offset[HID_GP_MAX_BUTTONS];

  bool has_hat;
  int16_t hat_offset;
  uint8_t hat_size;
  int32_t hat_min;
  int32_t hat_max;

  bool has_x;
  bool has_y;
  bool has_z; /* triggers, on pads that report them the DirectInput way */
  bool has_rz;
  int16_t x_offset;
  int16_t y_offset;
  int16_t z_offset;
  int16_t rz_offset;
  uint8_t axis_size;
  int32_t axis_min;
  int32_t axis_max;
} hid_gamepad_map_t;

typedef struct {
  uint32_t buttons; /* bit i set == button i pressed */
  int hat;          /* 0..7 clockwise from up, -1 when centred or absent */
  int32_t x;
  int32_t y;
  int32_t z;  /* 0 at rest, up to the axis maximum when pulled */
  int32_t rz;
} hid_gamepad_state_t;

// Parses a report descriptor. Returns the number of input reports found.
int hid_gamepad_parse(const uint8_t *desc, size_t len, hid_gamepad_map_t *maps, int max_maps);

// Finds the map for a report id, or the first map when the descriptor has no
// report ids at all.
const hid_gamepad_map_t *hid_gamepad_find(const hid_gamepad_map_t *maps, int count, uint16_t report_id);

// Decodes one input report into buttons/hat/axes.
void hid_gamepad_decode(const hid_gamepad_map_t *map, const uint8_t *data, size_t len,
                        hid_gamepad_state_t *out);
