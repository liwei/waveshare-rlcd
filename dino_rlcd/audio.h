// Sound effects for the dino game.
//
// Chromium synthesises these from triangle-wave oscillators rather than
// shipping audio files, in dino_game/generated_sound_fx.ts. We do the same:
// each cue is a list of notes (frequency, start offset, duration, volume) that
// a background task renders and streams to the on-board ES8311 codec.
#pragma once

enum AudioFx {
  FX_JUMP = 0,   // GeneratedSoundFx.jump()
  FX_SCORE,      // GeneratedSoundFx.collect()
  FX_HIT,        // GeneratedSoundFx.stopAll() -> cancelFootSteps()
};

// Brings up the codec and starts the mixing task. Safe to call even when no
// speaker is attached; playback simply goes nowhere.
bool Audio_Init();

// Queues a cue. Never blocks; a newer request replaces an unplayed one.
void Audio_Play(AudioFx fx);

// True once the codec opened and the mixer task is running.
bool Audio_Ready();
