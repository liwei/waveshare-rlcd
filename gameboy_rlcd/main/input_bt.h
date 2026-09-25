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

// Which Game Boy button a pad button maps to under the profile in force:
// 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB. Works with no pad connected.
uint8_t input_bt_map_probe(int which);
