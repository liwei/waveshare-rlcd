// Runtime audio engine selection, carried over from PaperBoy's audio.c. The
// square-wave buzzer engine was dropped along with the M5PaperS3 hardware.
#include "audio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "minigb_apu/minigb_apu.h"

// PCM backend, provided by audio_i2s.c.
void audio_pcm_init(void);
void audio_pcm_service_frame(void);
void audio_pcm_push_samples(const int16_t *samples, size_t n_pairs);
size_t audio_pcm_ring_free(void);
uint8_t audio_pcm_apu_read(uint16_t addr);
void audio_pcm_apu_write(uint16_t addr, uint8_t val);
void audio_pcm_deinit(void);
void audio_pcm_set_volume(int percent);
int audio_pcm_get_volume(void);
void audio_pcm_set_muted(bool muted);
bool audio_pcm_is_muted(void);
uint32_t audio_pcm_underruns(void);
int audio_pcm_ring_used(void);

static struct minigb_apu_ctx s_mute_apu;

static audio_engine_t s_engine = AUDIO_ENGINE_PCM;
static bool s_locked;

void audio_set_engine(audio_engine_t engine) {
  if (s_locked) {
    return;
  }
  if (engine < AUDIO_ENGINE_COUNT) {
    s_engine = engine;
  }
}

audio_engine_t audio_get_engine(void) { return s_engine; }

const char *audio_engine_name(audio_engine_t engine) {
  switch (engine) {
    case AUDIO_ENGINE_PCM:
      return "On";
    case AUDIO_ENGINE_MUTE:
      return "Mute";
    default:
      return "?";
  }
}

void audio_init(void) {
  s_locked = true;
  switch (s_engine) {
    case AUDIO_ENGINE_PCM:
      audio_pcm_init();
      break;
    default:
      minigb_apu_audio_init(&s_mute_apu);
      break;
  }
}

void audio_deinit(void) {
  if (s_engine == AUDIO_ENGINE_PCM) {
    audio_pcm_deinit();
  }
  s_locked = false;
}

void audio_service_frame(void) {
  if (s_engine == AUDIO_ENGINE_PCM) {
    audio_pcm_service_frame();
  }
}

void audio_push_samples(const int16_t *samples, size_t n_pairs) {
  if (s_engine == AUDIO_ENGINE_PCM) {
    audio_pcm_push_samples(samples, n_pairs);
  } else {
    (void)samples;
    (void)n_pairs;
  }
}

size_t audio_ring_free(void) {
  if (s_engine == AUDIO_ENGINE_PCM) {
    return audio_pcm_ring_free();
  }
  return (size_t)-1;
}

uint8_t audio_apu_read(uint16_t addr) {
  if (s_engine == AUDIO_ENGINE_PCM) {
    return audio_pcm_apu_read(addr);
  }
  return minigb_apu_audio_read(&s_mute_apu, addr);
}

void audio_apu_write(uint16_t addr, uint8_t val) {
  if (s_engine == AUDIO_ENGINE_PCM) {
    audio_pcm_apu_write(addr, val);
  } else {
    minigb_apu_audio_write(&s_mute_apu, addr, val);
  }
}

void audio_set_volume(int percent) {
  if (s_engine == AUDIO_ENGINE_PCM) {
    audio_pcm_set_volume(percent);
  }
}

int audio_get_volume(void) {
  if (s_engine == AUDIO_ENGINE_PCM) {
    return audio_pcm_get_volume();
  }
  return 0;
}

void audio_set_muted(bool muted) {
  if (s_engine == AUDIO_ENGINE_PCM) {
    audio_pcm_set_muted(muted);
  }
}

bool audio_is_muted(void) {
  if (s_engine == AUDIO_ENGINE_PCM) {
    return audio_pcm_is_muted();
  }
  return true;
}

uint32_t audio_underrun_count(void) {
  if (s_engine == AUDIO_ENGINE_PCM) {
    return audio_pcm_underruns();
  }
  return 0;
}

int audio_ring_used(void) {
  if (s_engine == AUDIO_ENGINE_PCM) {
    return audio_pcm_ring_used();
  }
  return 0;
}
