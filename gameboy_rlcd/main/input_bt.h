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

// What the mapping page offers: the Game Boy's own buttons, in the order they
// are listed and stored. The menu entry is the pad button that opens the pause
// menu, which is the one thing here the Game Boy itself has no equivalent for.
typedef enum {
  PAD_ACTION_A = 0,
  PAD_ACTION_B,
  PAD_ACTION_START,
  PAD_ACTION_SELECT,
  PAD_ACTION_UP,
  PAD_ACTION_DOWN,
  PAD_ACTION_LEFT,
  PAD_ACTION_RIGHT,
  PAD_ACTION_MENU,
  PAD_ACTION_COUNT
} pad_action_t;

#define PAD_BINDING_MAX 4

// A user mapping for one pad: which raw HID button index plays each Game Boy
// button, with -1 for "nothing assigned". Keyed by VID/PID, the same way the
// built-in profiles are chosen.
typedef struct {
  uint16_t vid;
  uint16_t pid;
  int8_t index[PAD_ACTION_COUNT];
} pad_binding_t;

// The button index an action currently uses, after any binding: -1 when it has
// none. Reads the effective mapping, so it reflects bindings immediately.
int8_t input_bt_action_get(int action);

// A human name for a raw button index under the profile in force, for menus and
// the console: "pad A", "Start", "Home", or "button 13" when the profile has no
// name for it. Never NULL.
const char *input_bt_index_name(int index);

// True when the connected pad reports its D-pad as a hat rather than buttons.
bool input_bt_has_hat(void);

// Remember a binding, and apply it at once when it belongs to the connected pad.
void input_bt_binding_set(const pad_binding_t *binding);

// Drop a binding, putting the built-in table for that model back in force.
void input_bt_binding_reset(uint16_t vid, uint16_t pid);

// Identity of the connected pad, used to key a binding. False when no pad is on.
bool input_bt_identity(uint16_t *vid, uint16_t *pid);

// Learn the next pad button pressed. While capturing, the pad drives nothing -
// not the menu and not the game - so the button being bound cannot also step
// the page underfoot.
void input_bt_capture_begin(void);
int input_bt_capture_take(void); /* -1 until a button is pressed, then its index */
void input_bt_capture_end(void);
