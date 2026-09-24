#include "audio.h"

#include <math.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "src/ExternLib/codec_board/codec_board.h"
#include "src/ExternLib/codec_board/codec_init.h"
#include "src/ExternLib/esp_codec_dev/include/esp_codec_dev.h"

#define TAG "audio"

// Matches the working configuration from Waveshare's 07_Audio_Test example
// (which writes 256-byte blocks from a 4 KB task at priority 2).
#define SAMPLE_RATE 24000
#define CHANNELS 2
#define BITS 16
#define BLOCK_FRAMES 60  // 2.5 ms per write, keeps latency low
#define SPEAKER_VOLUME 80
#define TASK_STACK 8192
#define TASK_PRIORITY 2

// ---------------------------------------------------------------------------
// Cue definitions, verbatim from dino_game/generated_sound_fx.ts
//
// playNote() runs two triangle oscillators detuned by +1 Hz and -2 Hz for a
// chorus effect, through one gain node. The gain sits at 0.1 for the body of
// the note, is set to `vol` 50 ms before the end, then ramps to silence.
// ---------------------------------------------------------------------------
struct Note {
  float freq;
  float start;   // seconds from cue start
  float dur;     // 0 marks the end of the cue
  float vol;
};

#define MAX_NOTES 4

static const Note CUE_JUMP[MAX_NOTES] = {
    {659.25f, 0.0f, 0.116f, 0.01f},
    {880.00f, 0.116f, 0.232f, 0.01f},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
};
static const Note CUE_SCORE[MAX_NOTES] = {
    {830.61f, 0.0f, 0.116f, 0.01f},
    {1318.51f, 0.116f, 0.232f, 0.01f},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
};
static const Note CUE_HIT[MAX_NOTES] = {
    {103.83f, 0.0f, 0.232f, 0.02f},
    {116.54f, 0.116f, 0.232f, 0.02f},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
};

static const Note *const CUES[3] = {CUE_JUMP, CUE_SCORE, CUE_HIT};

static volatile int s_pending = -1;
static esp_codec_dev_handle_t s_playback = nullptr;

static inline float triangle(float phase) {
  phase -= floorf(phase);
  return 4.0f * fabsf(phase - 0.5f) - 1.0f;  // -1 .. 1
}

static inline float envelope(float t, float dur, float vol) {
  const float fade = dur - 0.05f;
  if (t < fade) return 0.1f;
  if (t >= dur) return 0.0f;
  return vol + (0.00001f - vol) * ((t - fade) / (dur - fade));
}

static void audioTask(void *) {
  Note voices[MAX_NOTES];
  bool active[MAX_NOTES] = {false};
  int16_t block[BLOCK_FRAMES * CHANNELS];
  const double dt = 1.0 / SAMPLE_RATE;
  double now = 0.0;

  for (;;) {
    const int req = s_pending;
    if (req >= 0 && req < 3) {
      s_pending = -1;
      const Note *cue = CUES[req];
      for (int i = 0; i < MAX_NOTES; i++) {
        voices[i] = cue[i];
        voices[i].start = (float)now + cue[i].start;
        active[i] = cue[i].dur > 0.0f;
      }
    }

    for (int n = 0; n < BLOCK_FRAMES; n++) {
      float sum = 0.0f;
      for (int i = 0; i < MAX_NOTES; i++) {
        if (!active[i]) continue;
        const float lt = (float)now - voices[i].start;
        if (lt < 0.0f) continue;
        if (lt >= voices[i].dur) {
          active[i] = false;
          continue;
        }
        const float g = envelope(lt, voices[i].dur, voices[i].vol);
        sum += triangle((voices[i].freq + 1.0f) * lt) * g;
        sum += triangle((voices[i].freq - 2.0f) * lt) * g;
      }
      float v = sum * 32767.0f;
      if (v > 32767.0f) v = 32767.0f;
      if (v < -32768.0f) v = -32768.0f;
      block[n * 2] = (int16_t)v;      // same sample on both channels
      block[n * 2 + 1] = (int16_t)v;
      now += dt;
    }

    if (s_playback) {
      // Blocks on the I2S DMA and so paces the mixer; if it ever fails or
      // returns early, back off rather than spinning at full tilt.
      if (esp_codec_dev_write(s_playback, block, sizeof(block)) != ESP_CODEC_DEV_OK) {
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }

    // Rebase the clock whenever the mixer falls silent, so the note phases
    // never lose precision to a slowly growing absolute time.
    bool any = false;
    for (int i = 0; i < MAX_NOTES; i++) {
      if (active[i]) any = true;
    }
    if (!any && s_pending < 0) now = 0.0;
  }
}

bool Audio_Init() {
  set_codec_board_type("S3_RLCD_4_2");

  codec_init_cfg_t cfg = {};
  cfg.in_mode = CODEC_I2S_MODE_TDM;
  cfg.out_mode = CODEC_I2S_MODE_TDM;
  cfg.in_use_tdm = false;
  cfg.reuse_dev = false;
  if (init_codec(&cfg) != 0) {
    ESP_LOGE(TAG, "codec init failed");
    return false;
  }

  s_playback = get_playback_handle();
  if (!s_playback) {
    ESP_LOGE(TAG, "no playback handle");
    return false;
  }

  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = SAMPLE_RATE;
  fs.channel = CHANNELS;
  fs.bits_per_sample = BITS;
  if (esp_codec_dev_open(s_playback, &fs) != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "codec open failed");
    s_playback = nullptr;
    return false;
  }
  esp_codec_dev_set_out_vol(s_playback, SPEAKER_VOLUME);

  BaseType_t ok = xTaskCreatePinnedToCore(audioTask, "dino_audio", TASK_STACK, nullptr,
                                          TASK_PRIORITY, nullptr, 0);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "audio task failed");
    return false;
  }
  ESP_LOGI(TAG, "ready (%d Hz, %d ch)", SAMPLE_RATE, CHANNELS);
  return true;
}

void Audio_Play(AudioFx fx) { s_pending = (int)fx; }

bool Audio_Ready() { return s_playback != nullptr; }
