#include "battery.h"

#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"

static const char *TAG = "battery";

#define TREND_WINDOW 4
#define CHARGE_RISE_MV 25
#define CHARGE_AGREE 3

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_ready;

static int64_t s_last_sample_us;
static uint32_t s_mv;
static int s_percent;
static bool s_charging;
static bool s_low;

static uint32_t s_trend[TREND_WINDOW];
static int s_trend_count;
static int s_rise_run;
static int s_flat_run;

static uint32_t sample_mv(void) {
  uint32_t sum = 0;
  int samples = 0;

  for (int i = 0; i < 8; i++) {
    int raw = 0;
    if (adc_oneshot_read(s_adc, BATTERY_ADC_CHANNEL, &raw) != ESP_OK) {
      continue;
    }

    int mv = 0;
    if (s_cali != NULL) {
      if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
        continue;
      }
    } else {
      // Uncalibrated fallback for the 12 dB attenuation range.
      mv = (raw * 3100) / 4095;
    }

    sum += (uint32_t)mv;
    samples++;
  }

  if (samples == 0) {
    return s_mv;
  }

  // Undo the on-board divider so this reads pack voltage.
  return ((sum / (uint32_t)samples) * BATTERY_DIVIDER_SCALE);
}

void battery_init(void) {
  const adc_oneshot_unit_init_cfg_t unit_cfg = {
      .unit_id = BATTERY_ADC_UNIT,
  };
  if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
    ESP_LOGW(TAG, "ADC unit unavailable, battery gauge disabled");
    return;
  }

  const adc_oneshot_chan_cfg_t chan_cfg = {
      .atten = BATTERY_ADC_ATTEN,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  if (adc_oneshot_config_channel(s_adc, BATTERY_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
    ESP_LOGW(TAG, "ADC channel config failed");
    return;
  }

  const adc_cali_curve_fitting_config_t cali_cfg = {
      .unit_id = BATTERY_ADC_UNIT,
      .chan = BATTERY_ADC_CHANNEL,
      .atten = BATTERY_ADC_ATTEN,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
    s_cali = NULL;
    ESP_LOGW(TAG, "No ADC calibration, using a linear estimate");
  }

  s_ready = true;
  s_mv = sample_mv();
  s_percent = battery_percent();
  ESP_LOGI(TAG, "Battery: %lu mV", (unsigned long)s_mv);
}

void battery_update(void) {
  if (!s_ready) {
    return;
  }

  const int64_t now = esp_timer_get_time();
  if (s_last_sample_us != 0 &&
      (now - s_last_sample_us) < (int64_t)BATTERY_SAMPLE_INTERVAL_MS * 1000) {
    return;
  }
  s_last_sample_us = now;

  s_mv = sample_mv();

  if (s_trend_count < TREND_WINDOW) {
    s_trend[s_trend_count++] = s_mv;
  } else {
    memmove(&s_trend[0], &s_trend[1], sizeof(s_trend[0]) * (TREND_WINDOW - 1));
    s_trend[TREND_WINDOW - 1] = s_mv;
  }

  // The board exposes no charger status pin, so charging is inferred from a
  // sustained rise in pack voltage.
  if (s_trend_count == TREND_WINDOW) {
    const uint32_t oldest = s_trend[0];
    const uint32_t newest = s_trend[TREND_WINDOW - 1];
    if (newest > oldest && (newest - oldest) >= CHARGE_RISE_MV) {
      s_flat_run = 0;
      if (s_rise_run < CHARGE_AGREE) {
        s_rise_run++;
      }
    } else {
      s_rise_run = 0;
      if (s_flat_run < CHARGE_AGREE) {
        s_flat_run++;
      }
    }
    if (s_rise_run >= CHARGE_AGREE) {
      s_charging = true;
    } else if (s_flat_run >= CHARGE_AGREE) {
      s_charging = false;
    }
  }

  s_percent = battery_percent();
  s_low = (s_mv <= BATTERY_LOW_MV);
}

bool battery_valid(void) { return s_ready; }

uint32_t battery_millivolts(void) { return s_mv; }

int battery_percent(void) {
  const uint32_t mv = s_mv;
  if (mv <= 3000) {
    return 0;
  }
  if (mv >= BATTERY_FULL_MV) {
    return 100;
  }
  return (int)(((mv - 3000) * 100) / (BATTERY_FULL_MV - 3000));
}

bool battery_charging(void) { return s_charging; }

bool battery_is_low(void) { return s_ready && s_low; }
