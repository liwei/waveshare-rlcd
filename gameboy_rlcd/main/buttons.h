// KEY / BOOT buttons with a 2-of-N debounce, plus a hook the serial console
// uses to inject presses without touching the hardware.
#pragma once

#include <stdint.h>

#define BTN_KEY 0x01
#define BTN_BOOT 0x02

void buttons_init(void);

// Debounced state; call once per frame.
uint8_t buttons_poll(void);

// How long the current debounced state has been stable, in ms.
uint32_t buttons_hold_ms(void);

// Override levels for testing: `valid` selects which bits come from `pressed`
// instead of the pins. Pass valid = 0 to return to the hardware.
void buttons_override(uint8_t pressed, uint8_t valid);
