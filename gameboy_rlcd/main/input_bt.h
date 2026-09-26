// Bluetooth gamepad management: scan, connect, forget.
//
// Separate from input.h so the esp_hid definitions stay out of the emulator
// and UI code that only needs the resulting button mask.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_hid_gap.h"

// Brings up the BLE HID host and reconnects to the last pad, if any.
void input_bt_init(void);

// Starts a background scan; poll input_bt_scan_state() for completion.
void input_bt_scan_start(uint32_t seconds);
#define INPUT_BT_SCAN_IDLE 0
#define INPUT_BT_SCAN_RUNNING 1
#define INPUT_BT_SCAN_DONE 2
#define INPUT_BT_SCAN_FAILED 3
int input_bt_scan_state(void);

// Results of the last completed scan (owned by the caller until the next scan).
size_t input_bt_scan_results(esp_hid_scan_result_t **out);

void input_bt_connect(const esp_hid_scan_result_t *result);
void input_bt_forget(void);

// A one-line description of the scan state, for menus.
const char *input_bt_status_text(void);

// Log every input report that changes, to work out an unknown pad's layout.
void input_bt_set_verbose(bool on);

// The pad controls that can be re-bound. The order is pad_profile_t's field
// order and is written to disk, so append only.
typedef enum {
  PAD_FIELD_A = 0,
  PAD_FIELD_B,
  PAD_FIELD_X,
  PAD_FIELD_Y,
  PAD_FIELD_LB,
  PAD_FIELD_RB,
  PAD_FIELD_SELECT,
  PAD_FIELD_START,
  PAD_FIELD_GUIDE, /* the Home button; "guide" is the HID name for it */
  PAD_FIELD_DPAD_UP,
  PAD_FIELD_DPAD_DOWN,
  PAD_FIELD_DPAD_LEFT,
  PAD_FIELD_DPAD_RIGHT,
  PAD_FIELD_COUNT
} pad_field_t;

#define PAD_BINDING_MAX 4

// A user mapping for one pad model: one raw HID button index per control, with
// -1 for unmapped. Keyed by VID/PID, which is how the profiles are chosen.
typedef struct {
  uint16_t vid;
  uint16_t pid;
  int8_t field[PAD_FIELD_COUNT];
} pad_binding_t;

// The button index a control currently uses, after any binding: -1 when it has
// none. Reads the effective profile, so it reflects bindings immediately.
int8_t input_bt_profile_get(int field);

// Remember a binding, and apply it at once when it belongs to the connected pad.
void input_bt_binding_set(const pad_binding_t *binding);

// Drop a binding, putting the built-in profile for that model back in force.
void input_bt_binding_reset(uint16_t vid, uint16_t pid);

// Identity of the connected pad, used to key a binding. False when no pad is on.
bool input_bt_identity(uint16_t *vid, uint16_t *pid);

// Which Game Boy buttons a pad control produces, under the profile in force.
// Works with no pad connected.
uint8_t input_bt_map_probe(int field);

// Learn the next pad button pressed. While capturing, the pad drives nothing -
// not the menu and not the game - so the button being bound cannot also step
// the page underfoot.
void input_bt_capture_begin(void);
int input_bt_capture_take(void); /* -1 until a button is pressed, then its index */
void input_bt_capture_end(void);
