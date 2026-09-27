// Host-side test for the HID report descriptor parser.
//
// Build:  cc -I. -I../../gameboy_rlcd/main hosttest.c ../../gameboy_rlcd/main/hid_gamepad.c -o hosttest
#include <stdio.h>
#include <string.h>

#include "hid_gamepad.h"

static int failures;

#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      printf("FAIL: ");                                   \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
      failures++;                                         \
    } else {                                              \
      printf("ok  : ");                                   \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

// Verbatim 139-byte report descriptor from HandHeldLegend/SINPUT-LIB-HID, as
// shipped in ESP32-BLE-Gamepad. Report 1: 2 vendor bytes, 32 buttons, then six
// 16-bit axes (X, Y, Z, Rz, Rx, Ry).
static const uint8_t kSinput[139] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01, 0x06, 0x00, 0xFF, 0x09, 0x01, 0x15, 0x00, 0x25,
    0xFF, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0x05, 0x09, 0x19, 0x01, 0x29, 0x20, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x20, 0x81, 0x02, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09,
    0x35, 0x09, 0x33, 0x09, 0x34, 0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x06, 0x81,
    0x02, 0x06, 0x00, 0xFF, 0x09, 0x20, 0x15, 0x00, 0x26, 0xFF, 0xFF, 0x75, 0x20, 0x95, 0x01, 0x81,
    0x02, 0x09, 0x21, 0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x06, 0x81, 0x02, 0x09,
    0x22, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x1D, 0x81, 0x02, 0x85, 0x02, 0x09, 0x23,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x02, 0x85, 0x03, 0x09, 0x24, 0x15,
    0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x2F, 0x91, 0x02, 0xC0};

// A conventional gamepad report: report id 5, 10 buttons, a 4-bit hat, then
// 8-bit X and Y.
static const uint8_t kHatPad[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x05,        //   Report ID (5)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (1)
    0x29, 0x0A,        //   Usage Maximum (10)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0A,        //   Report Count (10)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x39,        //   Usage (Hat switch)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x07,        //   Logical Maximum (7)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x42,        //   Input (Data,Var,Abs,Null State)
    0x09, 0x30,        //   Usage (X)
    0x09, 0x31,        //   Usage (Y)
    0x15, 0x81,        //   Logical Minimum (-127)
    0x25, 0x7F,        //   Logical Maximum (127)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0xC0,              // End Collection
};

static void test_real_descriptor(void) {
  printf("\n== real 139-byte gamepad descriptor ==\n");
  hid_gamepad_map_t maps[HID_GP_MAX_REPORTS];
  const int count = hid_gamepad_parse(kSinput, sizeof(kSinput), maps, HID_GP_MAX_REPORTS);

  CHECK(count >= 1, "at least one report parsed (got %d)", count);
  if (count < 1) {
    return;
  }

  const hid_gamepad_map_t *r1 = hid_gamepad_find(maps, count, 1);
  CHECK(r1 != NULL, "report id 1 found");
  if (r1 == NULL) {
    return;
  }

  CHECK(r1->button_count == 32, "32 buttons (got %u)", r1->button_count);
  // 2 vendor bytes come first, so button 0 lives at bit 16.
  CHECK(r1->button_offset[0] == 16, "button 0 at bit 16 (got %d)", r1->button_offset[0]);
  CHECK(r1->button_count == 32, "all 32 buttons recorded");
  CHECK(r1->has_x && r1->x_offset == 48, "X at bit 48 (got %d)", r1->x_offset);
  CHECK(r1->has_y && r1->y_offset == 64, "Y at bit 64 (got %d)", r1->y_offset);
  CHECK(r1->axis_size == 16, "axes are 16 bit (got %u)", r1->axis_size);
  CHECK(r1->axis_min == -32768 && r1->axis_max == 32767, "axis range is signed 16 bit");

  // Report id 2 exists and carries no buttons or axes of interest.
  const hid_gamepad_map_t *r2 = hid_gamepad_find(maps, count, 2);
  CHECK(r2 != NULL, "report id 2 found");

  // Decode a report: byte 0-1 vendor, then buttons; press button 0 and 31.
  uint8_t report[64];
  memset(report, 0, sizeof(report));
  report[2] = 0x01;  // button 0  (bits 16..23)
  report[5] = 0x80;  // button 31 (bits 40..47)
  // X = 1000 (little endian at byte 6), Y = -1000 at byte 8.
  report[6] = 1000 & 0xFF;
  report[7] = (1000 >> 8) & 0xFF;
  const int16_t neg = -1000;
  report[8] = (uint8_t)(neg & 0xFF);
  report[9] = (uint8_t)((neg >> 8) & 0xFF);

  hid_gamepad_state_t state;
  hid_gamepad_decode(r1, report, sizeof(report), &state);
  CHECK((state.buttons & 0x1) != 0, "button 0 decoded pressed");
  CHECK((state.buttons & (1u << 31)) != 0, "button 31 decoded pressed");
  CHECK((state.buttons & 0x2) == 0, "button 1 decoded released");
  CHECK(state.x == 1000, "X decoded as 1000 (got %d)", state.x);
  CHECK(state.y == -1000, "Y decoded as -1000 (got %d)", state.y);
  CHECK(state.hat == -1, "no hat switch reported");
}

// Writes `value` into `bits` bits starting at `offset`, little-endian bit
// order, the way HID packs report fields.
static void set_bits(uint8_t *data, int offset, int bits, int32_t value) {
  for (int i = 0; i < bits; i++) {
    const int bit = offset + i;
    const uint8_t mask = (uint8_t)(1u << (bit & 7));
    if ((value >> i) & 1) {
      data[bit >> 3] |= mask;
    } else {
      data[bit >> 3] &= (uint8_t)~mask;
    }
  }
}

static void test_hat_descriptor(void) {
  printf("\n== gamepad with a hat switch ==\n");
  hid_gamepad_map_t maps[HID_GP_MAX_REPORTS];
  const int count = hid_gamepad_parse(kHatPad, sizeof(kHatPad), maps, HID_GP_MAX_REPORTS);

  const hid_gamepad_map_t *map = hid_gamepad_find(maps, count, 5);
  CHECK(map != NULL, "report id 5 found");
  if (map == NULL) {
    return;
  }
  CHECK(map->button_count == 10, "10 buttons (got %u)", map->button_count);
  CHECK(map->has_hat && map->hat_offset == 10, "hat at bit 10 (got %d)", map->hat_offset);
  CHECK(map->hat_size == 4, "hat is 4 bits (got %u)", map->hat_size);
  // 10 buttons + a 4-bit hat puts X at bit 14 and Y at bit 22.
  CHECK(map->has_x && map->x_offset == 14, "X at bit 14 (got %d)", map->x_offset);
  CHECK(map->has_y && map->y_offset == 22, "Y at bit 22 (got %d)", map->y_offset);

  // Walk every hat position plus the null state (8..15 -> centred).
  for (int pos = 0; pos <= 8; pos++) {
    uint8_t report[4] = {0, 0, 0, 0};
    // buttons 0 and 9, plus the hat in the top nibble of byte 1.
    report[0] = 0x01;
    report[1] = (uint8_t)(0x02 | ((pos & 0x0F) << 2));

    hid_gamepad_state_t state;
    hid_gamepad_decode(map, report, sizeof(report), &state);

    const int expected_hat = (pos <= 7) ? pos : -1;
    CHECK(state.hat == expected_hat, "hat %d decodes to %d (got %d)", pos, expected_hat, state.hat);
    CHECK((state.buttons & 0x1) != 0, "button 0 pressed with hat %d", pos);
    CHECK((state.buttons & (1u << 9)) != 0, "button 9 pressed with hat %d", pos);
  }

  // Axes, packed at their real (non byte-aligned) offsets.
  uint8_t report[4] = {0, 0, 0, 0};
  set_bits(report, map->x_offset, 8, 127);
  set_bits(report, map->y_offset, 8, -127);
  hid_gamepad_state_t state;
  hid_gamepad_decode(map, report, sizeof(report), &state);
  CHECK(state.x == 127, "X decoded as 127 (got %d)", state.x);
  CHECK(state.y == -127, "Y decoded as -127 (got %d)", state.y);

  // The X and Y fields overlap a byte boundary, so check a value that is
  // negative and one that is zero at the same time.
  memset(report, 0, sizeof(report));
  set_bits(report, map->x_offset, 8, -1);
  set_bits(report, map->y_offset, 8, 42);
  hid_gamepad_decode(map, report, sizeof(report), &state);
  CHECK(state.x == -1, "X decoded as -1 (got %d)", state.x);
  CHECK(state.y == 42, "Y decoded as 42 (got %d)", state.y);
}


// The Q36 in Android mode, exactly as it presents itself on the wire: a Consumer
// Control report and a Game Pad report, the latter with four 8-bit axes, a hat,
// sixteen buttons and two simulation axes. Its trigger axes matter beyond
// curiosity - L2 and R2 are Z and Rz, and the emulator's quick save and quick
// load hang off them - so this checks the offsets land on the right bytes.
static void test_q36_descriptor(void) {
  static const uint8_t descriptor[] = {
      0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x03, 0x75, 0x10, 0x95, 0x01, 0x15, 0x00, 0x26, 0x9c,
      0x02, 0x19, 0x01, 0x2a, 0x9c, 0x02, 0x81, 0x00, 0xc0, 0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85,
      0x04, 0x09, 0x01, 0xa1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26,
      0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0xc0, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35,
      0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x75, 0x04, 0x95, 0x01,
      0x81, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10,
      0x81, 0x02, 0x05, 0x02, 0x15, 0x00, 0x26, 0xff, 0x00, 0x09, 0xc4, 0x09, 0xc5, 0x95, 0x02, 0x75,
      0x08, 0x81, 0x02, 0x75, 0x08, 0x95, 0x01, 0x81, 0x01, 0xc0,
  };

  hid_gamepad_map_t maps[4];
  const int count = hid_gamepad_parse(descriptor, sizeof(descriptor), maps, 4);
  CHECK(count == 2, "Q36 descriptor yields 2 reports (got %d)", count);

  const hid_gamepad_map_t *pad = hid_gamepad_find(maps, count, 4);
  if (pad != NULL) {
    printf("      map: x=%d@%d y=%d@%d z=%d@%d rz=%d@%d hat=%d@%d size=%u\n", pad->has_x,
           pad->x_offset, pad->has_y, pad->y_offset, pad->has_z, pad->z_offset, pad->has_rz,
           pad->rz_offset, pad->has_hat, pad->hat_offset, (unsigned)pad->axis_size);
  }
  CHECK(pad != NULL && pad->has_x && pad->has_y, "the gamepad report has X and Y");
  CHECK(pad != NULL && pad->has_z && pad->has_rz, "and the trigger axes Z and Rz");
  CHECK(pad != NULL && pad->button_count == 16, "with 16 buttons (got %d)",
        pad != NULL ? pad->button_count : -1);

  // Byte 0 X, 1 Y, 2 Z, 3 Rz, 4 hat, 5-6 buttons, 7-8 simulation, 9 padding.
  uint8_t report[10] = {0};
  report[2] = 255; /* left trigger at the stop */
  hid_gamepad_state_t state;
  hid_gamepad_decode(pad, report, sizeof(report), &state);
  CHECK(state.z == 255, "Z decodes the left trigger (got %d)", state.z);
  CHECK(state.rz == 0, "and Rz stays released (got %d)", state.rz);
  CHECK(state.x == 0 && state.y == 0, "with both sticks centred");

  report[2] = 0;
  report[3] = 200;
  hid_gamepad_decode(pad, report, sizeof(report), &state);
  CHECK(state.rz == 200, "Rz decodes the right trigger (got %d)", state.rz);
}

int main(void) {
  test_real_descriptor();
  test_hat_descriptor();
  test_q36_descriptor();
  printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures,
         failures == 1 ? "" : "s");
  return failures != 0;
}
