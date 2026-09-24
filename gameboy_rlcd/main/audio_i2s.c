// PCM audio backend: minigb_apu -> ring buffer -> I2S -> ES8311.
//
// The APU produces AUDIO_SAMPLES stereo pairs per Game Boy frame at
// AUDIO_SAMPLE_RATE; that is down-mixed to mono and drained to the codec by a
// task on core 0, leaving the emulator free to run on core 1.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "codec_board.h"
#include "codec_init.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"
#include "config.h"
#include "minigb_apu/minigb_apu.h"

static const char *TAG = "audio";

// Mono ring buffer. Power of two so the indices can wrap with a mask.
#define RING_SAMPLES 4096
#define RING_MASK (RING_SAMPLES - 1)
#define PLAYBACK_BLOCK 256

static int16_t s_ring[RING_SAMPLES];
static volatile int s_head; /* producer */
static volatile int s_tail; /* consumer */

static struct minigb_apu_ctx s_apu;
static int16_t s_chunk[AUDIO_SAMPLES_TOTAL];

static esp_codec_dev_handle_t s_dev;
static TaskHandle_t s_task;
static volatile bool s_running;
static volatile int s_pending;
static volatile uint32_t s_underruns;
static int s_volume = AUDIO_VOLUME_DEFAULT;
static volatile bool s_muted;
static bool s_ready;

static int ring_used(void) {
  int used = s_head - s_tail;
  if (used < 0) {
    used += RING_SAMPLES;
  }
  return used;
}

static int ring_pop(int16_t *dst, int max) {
  int n = 0;
  while (n < max && s_tail != s_head) {
    dst[n++] = s_ring[s_tail];
    s_tail = (s_tail + 1) & RING_MASK;
  }
  return n;
}

// Drains the ring to the codec. Paced by the I2S DMA, so an empty ring means
// the emulator is behind and the gap is filled with silence rather than
// stretching the existing samples.
static void audio_task(void *arg) {
  (void)arg;
  static int16_t block[PLAYBACK_BLOCK * 2];

  while (s_running) {
    int16_t mono[PLAYBACK_BLOCK];
    int got = ring_pop(mono, PLAYBACK_BLOCK);
    if (got < PLAYBACK_BLOCK) {
      memset(&mono[got], 0, sizeof(int16_t) * (size_t)(PLAYBACK_BLOCK - got));
      if (s_running && s_head != s_tail) {
        // partially refilled, not a real underrun
      } else if (s_running) {
        s_underruns++;
      }
    }

    if (s_muted) {
      memset(mono, 0, sizeof(mono));
    }

    for (int i = 0; i < PLAYBACK_BLOCK; i++) {
      block[2 * i] = mono[i];
      block[2 * i + 1] = mono[i];
    }

    if (esp_codec_dev_write(s_dev, block, sizeof(block)) != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  s_task = NULL;
  vTaskDelete(NULL);
}

static void audio_pcm_init_ring(void) {
  s_head = 0;
  s_tail = 0;
  s_pending = 0;
  s_underruns = 0;
}

bool audio_pcm_is_ready(void) { return s_ready; }

void audio_pcm_init(void) {
  audio_pcm_init_ring();
  minigb_apu_audio_init(&s_apu);

  set_codec_board_type("S3_RLCD_4_2");

  // codec_board only creates the I2C bus when asked; the ES8311 is configured
  // over it.
  if (init_i2c(CODEC_I2C_PORT) != 0) {
    ESP_LOGE(TAG, "I2C init failed; audio disabled");
    return;
  }

  codec_init_cfg_t cfg = {
      .in_mode = CODEC_I2S_MODE_TDM,
      .out_mode = CODEC_I2S_MODE_TDM,
      .in_use_tdm = false,
      .reuse_dev = false,
  };
  if (init_codec(&cfg) != 0) {
    ESP_LOGE(TAG, "codec init failed; audio disabled");
    return;
  }

  s_dev = get_playback_handle();
  if (s_dev == NULL) {
    ESP_LOGE(TAG, "no playback handle; audio disabled");
    return;
  }

  const esp_codec_dev_sample_info_t fs = {
      .sample_rate = AUDIO_SAMPLE_RATE,
      .channel = 2,
      .bits_per_sample = 16,
  };
  if (esp_codec_dev_open(s_dev, &fs) != ESP_OK) {
    ESP_LOGE(TAG, "codec open failed; audio disabled");
    s_dev = NULL;
    return;
  }

  esp_codec_dev_set_out_vol(s_dev, s_volume);
  s_ready = true;
  s_running = true;

  if (xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 3, &s_task, 0) != pdPASS) {
    ESP_LOGE(TAG, "audio task create failed");
    s_ready = false;
    s_running = false;
    return;
  }

  ESP_LOGI(TAG, "ES8311 ready at %d Hz, %d samples/frame", AUDIO_SAMPLE_RATE, AUDIO_SAMPLES);
}

void audio_pcm_service_frame(void) {
  if (!s_ready) {
    return;
  }

  s_pending += AUDIO_SAMPLES;
  if (s_pending > AUDIO_SAMPLES * 4) {
    s_pending = AUDIO_SAMPLES * 4; /* drop if the emulator raced ahead */
  }

  while (s_pending >= AUDIO_SAMPLES && (RING_SAMPLES - 1 - ring_used()) >= AUDIO_SAMPLES) {
    minigb_apu_audio_callback(&s_apu, s_chunk);
    for (int i = 0; i < AUDIO_SAMPLES; i++) {
      const int32_t mix = ((int32_t)s_chunk[2 * i] + (int32_t)s_chunk[2 * i + 1]) / 2;
      s_ring[s_head] = (int16_t)mix;
      s_head = (s_head + 1) & RING_MASK;
    }
    s_pending -= AUDIO_SAMPLES;
  }
}

void audio_pcm_push_samples(const int16_t *samples, size_t n_pairs) {
  if (!s_ready) {
    return;
  }

  const int free_slots = RING_SAMPLES - 1 - ring_used();
  const size_t count = (n_pairs < (size_t)free_slots) ? n_pairs : (size_t)free_slots;
  for (size_t i = 0; i < count; i++) {
    const int32_t mix = ((int32_t)samples[2 * i] + (int32_t)samples[2 * i + 1]) / 2;
    s_ring[s_head] = (int16_t)mix;
    s_head = (s_head + 1) & RING_MASK;
  }
}

size_t audio_pcm_ring_free(void) { return (size_t)(RING_SAMPLES - 1 - ring_used()); }

uint8_t audio_pcm_apu_read(uint16_t addr) { return minigb_apu_audio_read(&s_apu, addr); }

void audio_pcm_apu_write(uint16_t addr, uint8_t val) { minigb_apu_audio_write(&s_apu, addr, val); }

void audio_pcm_set_volume(int percent) {
  if (percent < 0) {
    percent = 0;
  }
  if (percent > 100) {
    percent = 100;
  }
  s_volume = percent;
  if (s_ready && s_dev != NULL) {
    esp_codec_dev_set_out_vol(s_dev, s_volume);
  }
}

int audio_pcm_get_volume(void) { return s_volume; }

void audio_pcm_set_muted(bool muted) { s_muted = muted; }

bool audio_pcm_is_muted(void) { return s_muted; }

uint32_t audio_pcm_underruns(void) { return s_underruns; }

int audio_pcm_ring_used(void) { return ring_used(); }

void audio_pcm_deinit(void) {
  if (!s_ready) {
    return;
  }

  s_running = false;
  while (s_task != NULL) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (s_dev != NULL) {
    esp_codec_dev_close(s_dev);
    s_dev = NULL;
  }
  deinit_codec();
  s_ready = false;
}
