#include "buttons.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

// A level change must be seen this many polls in a row to become stable. At
// 60 Hz that is ~33 ms of contact bounce rejection.
#define BTN_AGREE_POLLS 2

static uint8_t s_stable;
static uint8_t s_candidate;
static uint8_t s_agree;
static int64_t s_changed_us;

static uint8_t s_ovr_pressed;
static uint8_t s_ovr_valid;

static uint8_t read_raw(void) {
  uint8_t mask = 0;

  if (s_ovr_valid & BTN_KEY) {
    mask |= (s_ovr_pressed & BTN_KEY);
  } else if (gpio_get_level(BTN_KEY_PIN) == 0) {
    mask |= BTN_KEY;
  }

  if (s_ovr_valid & BTN_BOOT) {
    mask |= (s_ovr_pressed & BTN_BOOT);
  } else if (gpio_get_level(BTN_BOOT_PIN) == 0) {
    mask |= BTN_BOOT;
  }

  return mask;
}

void buttons_init(void) {
  const gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << BTN_KEY_PIN) | (1ULL << BTN_BOOT_PIN),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&cfg);

  vTaskDelay(pdMS_TO_TICKS(20));
  s_stable = read_raw();
  s_candidate = s_stable;
  s_agree = BTN_AGREE_POLLS;
  s_changed_us = esp_timer_get_time();
}

uint8_t buttons_poll(void) {
  const uint8_t raw = read_raw();

  if (raw == s_candidate) {
    if (s_agree < BTN_AGREE_POLLS) {
      s_agree++;
    }
  } else {
    s_candidate = raw;
    s_agree = 1;
  }

  if (s_agree >= BTN_AGREE_POLLS && s_candidate != s_stable) {
    s_stable = s_candidate;
    s_changed_us = esp_timer_get_time();
  }

  return s_stable;
}

uint32_t buttons_hold_ms(void) {
  const int64_t elapsed = (esp_timer_get_time() - s_changed_us) / 1000;
  return (elapsed > 0) ? (uint32_t)elapsed : 0;
}

void buttons_override(uint8_t pressed, uint8_t valid) {
  s_ovr_pressed = pressed;
  s_ovr_valid = valid;
}
