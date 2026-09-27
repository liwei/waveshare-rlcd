// Merged input: the two physical buttons plus a Bluetooth gamepad.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void input_init(void);

// Current pressed mask (GB_BTN_*), for gameplay.
uint8_t input_read(void);

// Menu-friendly events: rising edges, with auto-repeat on up/down, plus A as
// "select" and B as "back".
uint8_t input_menu_events(void);

// Forget the current button state, so buttons still held while a menu opens
// (the pause chord, for instance) are not seen as a fresh press.
void input_menu_reset(void);

// True once per press-and-hold of both physical buttons.
bool input_pause_requested(void);

// Gamepad state, provided by input_bt.c.
uint8_t input_bt_buttons(void);
bool input_bt_connected(void);
const char *input_bt_name(void);

// True once per press of the gamepad's guide button (consumes the edge).
bool input_bt_take_menu(void);

// One-shot requests from the gamepad's triggers: R2 asks for a quick save, L2
// for a quick load. True once per pull.
bool input_bt_take_quick_save(void);
bool input_bt_take_quick_load(void);
