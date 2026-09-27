#include "input.h"

#include <string.h>

#include "esp_timer.h"

#include "buttons.h"
#include "config.h"
#include "gbemu.h"

// Auto-repeat for menu navigation.
#define REPEAT_DELAY_MS 450
#define REPEAT_RATE_MS 140

static uint8_t s_prev;
static uint8_t s_repeat_btn;
static int64_t s_repeat_next_ms;
static bool s_chord_fired;
static bool s_spend_chord;
static bool s_pause;

static uint8_t physical_buttons(uint8_t raw) {
  uint8_t mask = 0;

  // With no gamepad connected the two buttons stand in for A and B; when one is
  // connected they keep the same roles so the device stays usable either way.
  if (raw & BTN_KEY) {
    mask |= GB_BTN_A;
  }
  if (raw & BTN_BOOT) {
    mask |= GB_BTN_B;
  }

  return mask;
}

void input_init(void) {
  buttons_init();
  s_prev = 0;
  s_chord_fired = false;
}

uint8_t input_read(void) {
  const uint8_t raw = buttons_poll();

  // Both buttons held, or the pad's guide button: pause. Consumed until
  // released so a held chord does not keep re-opening the menu.
  s_pause = false;
  if (raw == (BTN_KEY | BTN_BOOT)) {
    if (!s_chord_fired && buttons_hold_ms() >= BTN_CHORD_MS) {
      s_chord_fired = true;
      s_pause = true;
      s_spend_chord = true;
    }
  } else {
    s_chord_fired = false;
  }

  if (input_bt_take_menu()) {
    s_pause = true;
  }

  uint8_t physical = physical_buttons(raw);

  // The chord that opened the menu is spent on it: while either button is still
  // down it contributes nothing to the game, so resuming does not hand the game
  // a held A+B. It only ever applies to a chord that actually opened the menu,
  // so holding both buttons during play still works as A+B.
  if (s_spend_chord) {
    physical &= (uint8_t)~(GB_BTN_A | GB_BTN_B);
    if ((raw & (BTN_KEY | BTN_BOOT)) == 0) {
      s_spend_chord = false;
    }
  }

  return (uint8_t)(physical | input_bt_buttons());
}

bool input_pause_requested(void) { return s_pause; }

// In menus the two physical buttons cover navigation and selection: KEY steps
// through the entries and BOOT activates them. The list wraps, so any entry is
// reachable by going round, and holding both goes back up a level — which is
// the only way out of menus that have no exit entry of their own, such as the
// scan results. While both are down their individual meanings are suppressed,
// otherwise reaching for "back" would first step the cursor and activate
// whatever it landed on. A connected gamepad keeps its own mapping on top.
static uint8_t menu_buttons(void) {
  const uint8_t raw = buttons_poll();
  uint8_t cur = input_bt_buttons();

  // A chord pressed while a menu is already up is spent on that menu. The
  // emulator is not running to consume it, so without this it would still be
  // pending when play resumes and would re-open the pause menu straight away.
  (void)input_bt_take_menu();

  // Same for the triggers: a quick save asked for during a menu is not wanted
  // the moment play resumes.
  (void)input_bt_take_quick_save();
  (void)input_bt_take_quick_load();

  const uint32_t held = buttons_hold_ms();

  if (raw == (BTN_KEY | BTN_BOOT)) {
    if (held >= BTN_CHORD_MS) {
      cur |= GB_BTN_B;
    }
    return cur;
  }

  // Give the other button a chance to arrive before this one acts alone: the
  // individual meanings are "step" and "activate", so a chord that is a little
  // out of step would otherwise walk the cursor and open whatever it landed on
  // instead of going back.
  if (raw != 0 && held < BTN_CHORD_GRACE_MS) {
    return cur;
  }

  if (raw & BTN_KEY) {
    cur |= GB_BTN_DOWN;
  }
  if (raw & BTN_BOOT) {
    cur |= GB_BTN_A;
  }

  return cur;
}

void input_menu_reset(void) {
  s_prev = menu_buttons();
  s_repeat_btn = 0;
}

uint8_t input_menu_events(void) {
  const uint8_t cur = menu_buttons();
  uint8_t edge = (uint8_t)(cur & ~s_prev);
  const int64_t now_ms = esp_timer_get_time() / 1000;

  const uint8_t nav = cur & (GB_BTN_UP | GB_BTN_DOWN);
  if (nav != 0) {
    if (s_repeat_btn == 0) {
      s_repeat_btn = nav;
      s_repeat_next_ms = now_ms + REPEAT_DELAY_MS;
    } else if (now_ms >= s_repeat_next_ms) {
      edge |= s_repeat_btn;
      s_repeat_next_ms = now_ms + REPEAT_RATE_MS;
    }
  } else {
    s_repeat_btn = 0;
  }

  s_prev = cur;
  return edge;
}
