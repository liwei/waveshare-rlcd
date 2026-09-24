// Audio output: PCM through the on-board ES8311 codec, or muted.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  AUDIO_ENGINE_PCM = 0, /*!< ES8311 over I2S */
  AUDIO_ENGINE_MUTE = 1, /*!< no hardware output; APU state still maintained */
  AUDIO_ENGINE_COUNT,
} audio_engine_t;

// Must be called before audio_init(); frozen afterwards.
void audio_set_engine(audio_engine_t engine);
audio_engine_t audio_get_engine(void);
const char *audio_engine_name(audio_engine_t engine);

void audio_init(void);
void audio_deinit(void);

// Drive one Game Boy frame's worth of synthesis; call once per emulated frame.
void audio_service_frame(void);

void audio_push_samples(const int16_t *samples, size_t n_pairs);
size_t audio_ring_free(void);

// APU register access on behalf of the emulated CPU (0xFF10-0xFF3F).
uint8_t audio_apu_read(uint16_t addr);
void audio_apu_write(uint16_t addr, uint8_t val);

// 0..100, forwarded to the codec's output volume.
void audio_set_volume(int percent);
int audio_get_volume(void);

// Silence the output without tearing the codec down.
void audio_set_muted(bool muted);
bool audio_is_muted(void);

// Diagnostics: samples the playback task had to fill with silence.
uint32_t audio_underrun_count(void);
int audio_ring_used(void);
