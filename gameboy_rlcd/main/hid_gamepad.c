#include "hid_gamepad.h"

#include <string.h>

#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "hid";

#define USAGE_PAGE_GENERIC_DESKTOP 0x01
#define USAGE_PAGE_BUTTON 0x09

#define USAGE_X 0x30
#define USAGE_Y 0x31
#define USAGE_Z 0x32  /* left trigger, on pads that report triggers as axes */
#define USAGE_RZ 0x35 /* right trigger */
#define USAGE_HAT_SWITCH 0x39

// One entry per report id seen in the descriptor. All the input items that make
// up a report append their fields to the same map.
typedef struct {
  int report_id;
  int map_index;
  int bit_offset;
} report_state_t;

static uint32_t read_item(const uint8_t *desc, size_t len, size_t *pos, int size) {
  uint32_t value = 0;
  for (int i = 0; i < size; i++) {
    if (*pos < len) {
      value |= (uint32_t)desc[*pos] << (8 * i);
    }
    (*pos)++;
  }
  return value;
}

// HID fields are packed little-endian, lowest bit of byte 0 first.
static int32_t extract_field(const uint8_t *data, size_t len, int offset, int size, bool is_signed) {
  uint32_t value = 0;

  if (size <= 0 || size > 32) {
    return 0;
  }

  for (int i = 0; i < size; i++) {
    const int bit = offset + i;
    if ((bit >> 3) >= (int)len) {
      break;
    }
    if (data[bit >> 3] & (1u << (bit & 7))) {
      value |= (1u << i);
    }
  }

  if (is_signed && size < 32 && (value & (1u << (size - 1))) != 0) {
    value |= ~((1u << size) - 1u);
  }

  return (int32_t)value;
}

static int32_t sign_extend(uint32_t value, int size) {
  if (size == 0) {
    return 0;
  }
  if (size < 32 && (value & (1u << (size - 1))) != 0) {
    value |= ~((1u << size) - 1u);
  }
  return (int32_t)value;
}

int hid_gamepad_parse(const uint8_t *desc, size_t len, hid_gamepad_map_t *maps, int max_maps) {
  uint32_t usage_page = 0;
  uint32_t report_size = 0;
  uint32_t report_count = 0;
  uint32_t report_id = 0;
  int32_t logical_min = 0;
  int32_t logical_max = 0;

  uint32_t usages[32];
  int usage_count = 0;
  uint32_t usage_min = 0;
  uint32_t usage_max = 0;
  bool have_usage_range = false;

  report_state_t reports[HID_GP_MAX_REPORTS];
  int report_count_seen = 0;
  int map_count = 0;

  memset(maps, 0, sizeof(hid_gamepad_map_t) * (size_t)max_maps);

  size_t pos = 0;
  while (pos < len) {
    const uint8_t prefix = desc[pos++];
    const int size = (prefix & 0x03) == 3 ? 4 : (prefix & 0x03);
    const int type = (prefix >> 2) & 0x03;
    const int tag = (prefix >> 4) & 0x0F;

    if (type == 1) { /* Global */
      switch (tag) {
        case 0x0:
          usage_page = read_item(desc, len, &pos, size);
          break;
        case 0x1:
          logical_min = sign_extend(read_item(desc, len, &pos, size), size * 8);
          break;
        case 0x2:
          logical_max = sign_extend(read_item(desc, len, &pos, size), size * 8);
          break;
        case 0x7:
          report_size = read_item(desc, len, &pos, size);
          break;
        case 0x8:
          report_id = read_item(desc, len, &pos, size);
          break;
        case 0x9:
          report_count = read_item(desc, len, &pos, size);
          break;
        default:
          read_item(desc, len, &pos, size);
          break;
      }
      continue;
    }

    if (type == 2) { /* Local */
      switch (tag) {
        case 0x0: {
          const uint32_t value = read_item(desc, len, &pos, size);
          if (usage_count < (int)(sizeof(usages) / sizeof(usages[0]))) {
            usages[usage_count++] = value;
          }
          break;
        }
        case 0x1:
          usage_min = read_item(desc, len, &pos, size);
          have_usage_range = true;
          break;
        case 0x2:
          usage_max = read_item(desc, len, &pos, size);
          break;
        default:
          read_item(desc, len, &pos, size);
          break;
      }
      continue;
    }

    if (type != 0) { /* Reserved */
      read_item(desc, len, &pos, size);
      continue;
    }

    /* Main item */
    if (tag == 0xa || tag == 0xb) { /* Collection, End Collection */
      // A collection is a Main item, so it clears the local items around it like
      // any other. Its own usage - "Game Pad", "Pointer" - would otherwise sit
      // in front of the fields that follow and shift every one of them along,
      // which is exactly what it did: a pad declaring X, Y, Z and Rz in one
      // Input item had its sticks read out of the trigger bytes.
      read_item(desc, len, &pos, size);
      usage_count = 0;
      have_usage_range = false;
      continue;
    }

    if (tag == 0x8) { /* Input */
      int state = -1;
      for (int i = 0; i < report_count_seen; i++) {
        if (reports[i].report_id == (int)report_id) {
          state = i;
          break;
        }
      }

      if (state < 0) {
        if (report_count_seen >= HID_GP_MAX_REPORTS || map_count >= max_maps) {
          usage_count = 0;
          have_usage_range = false;
          continue;
        }
        state = report_count_seen++;
        reports[state].report_id = (int)report_id;
        reports[state].map_index = map_count++;
        reports[state].bit_offset = 0;

        hid_gamepad_map_t *fresh = &maps[reports[state].map_index];
        fresh->report_id = (uint8_t)report_id;
        fresh->valid = true;
      }

      hid_gamepad_map_t *map = &maps[reports[state].map_index];
      int *offset = &reports[state].bit_offset;

      for (uint32_t i = 0; i < report_count; i++) {
        uint32_t usage = 0;
        bool have_usage = false;

        if (i < (uint32_t)usage_count) {
          usage = usages[i];
          have_usage = true;
        } else if (have_usage_range && (usage_min + i) <= usage_max) {
          usage = usage_min + i;
          have_usage = true;
        }

        if (have_usage) {
          if (usage_page == USAGE_PAGE_BUTTON) {
            if (map->button_count < HID_GP_MAX_BUTTONS) {
              map->button_offset[map->button_count] = (int16_t)(*offset);
              map->button_count++;
            }
          } else if (usage_page == USAGE_PAGE_GENERIC_DESKTOP) {
            if (usage == USAGE_HAT_SWITCH && !map->has_hat) {
              map->has_hat = true;
              map->hat_offset = (int16_t)(*offset);
              map->hat_size = (uint8_t)report_size;
              map->hat_min = logical_min;
              map->hat_max = logical_max;
            } else if (usage == USAGE_X && !map->has_x) {
              map->has_x = true;
              map->x_offset = (int16_t)(*offset);
              map->axis_size = (uint8_t)report_size;
              map->axis_min = logical_min;
              map->axis_max = logical_max;
            } else if (usage == USAGE_Z && !map->has_z) {
              map->has_z = true;
              map->z_offset = (int16_t)(*offset);
            } else if (usage == USAGE_RZ && !map->has_rz) {
              map->has_rz = true;
              map->rz_offset = (int16_t)(*offset);
            } else if (usage == USAGE_Y && !map->has_y) {
              map->has_y = true;
              map->y_offset = (int16_t)(*offset);
              map->axis_size = (uint8_t)report_size;
              map->axis_min = logical_min;
              map->axis_max = logical_max;
            }
          }
        }

        *offset += (int)report_size;
      }

      /* Input items carry data (the flag byte); skipping it would desync the
       * rest of the descriptor. */
      (void)read_item(desc, len, &pos, size);

      usage_count = 0;
      have_usage_range = false;
      continue;
    }

    if (tag == 0xA) { /* Collection */
      read_item(desc, len, &pos, size);
      continue;
    }

    /* Output, Feature and EndCollection carry no local state reset. */
    read_item(desc, len, &pos, size);
  }

  int usable = 0;
  for (int i = 0; i < map_count; i++) {
    if (maps[i].button_count > 0 || maps[i].has_hat || (maps[i].has_x && maps[i].has_y)) {
      usable++;
    }
    ESP_LOGI(TAG, "report %u: %u buttons, hat %s, axes %s (%d bits used)", maps[i].report_id,
             maps[i].button_count, maps[i].has_hat ? "yes" : "no",
             (maps[i].has_x && maps[i].has_y) ? "yes" : "no",
             reports[i < report_count_seen ? i : 0].bit_offset);
  }
  ESP_LOGI(TAG, "report descriptor parsed: %d report(s), %d usable", map_count, usable);

  return map_count;
}

const hid_gamepad_map_t *hid_gamepad_find(const hid_gamepad_map_t *maps, int count,
                                          uint16_t report_id) {
  for (int i = 0; i < count; i++) {
    if (maps[i].valid && maps[i].report_id == report_id) {
      return &maps[i];
    }
  }
  /* A descriptor without report ids only has one report. */
  if (count > 0 && maps[0].valid) {
    return &maps[0];
  }
  return NULL;
}

void hid_gamepad_decode(const hid_gamepad_map_t *map, const uint8_t *data, size_t len,
                        hid_gamepad_state_t *out) {
  memset(out, 0, sizeof(*out));
  out->hat = -1;

  if (map == NULL) {
    return;
  }

  for (int i = 0; i < map->button_count; i++) {
    if (extract_field(data, len, map->button_offset[i], 1, false) != 0) {
      out->buttons |= (1u << i);
    }
  }

  if (map->has_hat && map->hat_size > 0) {
    const int32_t raw = extract_field(data, len, map->hat_offset, map->hat_size, false);
    const int span = (int)(map->hat_max - map->hat_min) + 1;
    const int position = (int)(raw - map->hat_min);

    if (raw >= map->hat_min && raw <= map->hat_max) {
      /* A nine-value range carries a null state in its last slot. */
      out->hat = (span > 8 && position == span - 1) ? -1 : position;
    } else {
      out->hat = -1;
    }
  }

  if (map->has_x) {
    out->x = extract_field(data, len, map->x_offset, map->axis_size, map->axis_min < 0);
  }
  if (map->has_y) {
    out->y = extract_field(data, len, map->y_offset, map->axis_size, map->axis_min < 0);
  }
  if (map->has_z) {
    out->z = extract_field(data, len, map->z_offset, map->axis_size, map->axis_min < 0);
  }
  if (map->has_rz) {
    out->rz = extract_field(data, len, map->rz_offset, map->axis_size, map->axis_min < 0);
  }
}
