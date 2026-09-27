# Chromium dino game — Waveshare ESP32-S3-RLCD-4.2

A port of Chrome's offline T-Rex runner to the Waveshare ESP32-S3-RLCD-4.2
(ESP32-S3-WROOM-1-N16R8 + 4.2" 300×400 reflective LCD). It is one of the two
projects in this repository — the other is the
[Game Boy emulator](../gameboy_rlcd/README.md), which is what the
[top-level README](../README.md) introduces.

**Controls**

| Button | Action |
| --- | --- |
| **KEY** (GPIO18) | Jump — tap for a short hop, hold for a full jump. Starts the game and restarts after a crash. |
| **BOOT** (GPIO0) | Pause / resume. Also starts the game. |

## Artwork and logic are Chromium's, verbatim

Nothing is redrawn or re-tuned by hand.

* **Artwork** — every bitmap is extracted pixel-for-pixel from the official
  `components/neterror/resources/images/default_100_percent/offline/100-offline-sprite.png`.
  The crop coordinates come straight from `dino_game/offline_sprite_definitions.ts`
  (`ldpi`), including the cactus clustering trick where a size-N cluster is a
  single wider sprite at `sourceX = spritePos.x + width*size*0.5*(size-1)`.
* **Logic** — gameplay constants and rules are ported from
  `dino_game/{offline,trex,obstacle,horizon,distance_meter,cloud,night_mode}.ts`:

  | | |
  | --- | --- |
  | `ACCELERATION` 0.001, `SPEED` 6, `MAX_SPEED` 13 | `GRAVITY` 0.6, `INIITAL_JUMP_VELOCITY` -10 |
  | `DROP_VELOCITY` -5, `SPEED_DROP_COEFFICIENT` 3 | `MIN`/`MAX_JUMP_HEIGHT` 30 |
  | `GAP_COEFFICIENT` 0.6, `MAX_GAP_COEFFICIENT` 1.5 | `MAX_OBSTACLE_LENGTH` 3, `MAX_OBSTACLE_DUPLICATION` 2 |
  | `COEFFICIENT` 0.025, `ACHIEVEMENT_DISTANCE` 100 | `INVERT_DISTANCE` 700, fade 12000 ms |
  | `BG_CLOUD_SPEED` 0.2, `MAX_CLOUDS` 6 | `START_X_POS` 50, `BOTTOM_PAD` 10 |

  Collision boxes, obstacle `minGap`/`multipleSpeed`/`minSpeed` gating, the
  `getGap()` widening, the running/blinking/crashed animation frame rates, the
  zero-padded 5-digit score, the `HI` prefix, the score flash on every 100
  points and the day/night inversion are all reproduced as in the original.
* **Sound** — `generated_sound_fx.ts` synthesises its cues from triangle-wave
  oscillators rather than shipping audio files, and so does this port
  (`audio.cpp`): two oscillators detuned by +1 Hz and −2 Hz through a gain node
  that sits at 0.1 and fades over the last 50 ms. Played through the on-board
  ES8311 via `esp_codec_dev` + Waveshare's `codec_board` (vendored under `src/`).

  | Cue | Notes | Trigger |
  | --- | --- | --- |
  | `jump()` | 659.25 Hz 116 ms → 880 Hz 232 ms | KEY press |
  | `collect()` | 830.61 Hz 116 ms → 1318.51 Hz 232 ms | every 100 points |
  | `stopAll()` | 103.83 Hz 232 ms → 116.54 Hz 232 ms | crash |

  The `background()` jingle and `loopFootSteps()` cues are omitted: those belong
  to Chromium's audio-cue accessibility mode (a footstep thump every 280 ms) and
  two of the notes sit near 70 Hz, below what the MX1.25 speaker can reproduce.

### Three deliberate deviations

1. **No ducking.** The board has only two buttons and both are spoken for, so
   the pterodactyl uses the original's `yPosMobile` heights `[100, 50]` —
   exactly what Chromium does on touch devices that cannot duck either. Both
   heights stay meaningful: the low one must be jumped, the high one must *not*
   be.
2. **Spawn edge is 400 px, not 600.** The panel is narrower than the original
   600 px canvas, so obstacles enter at the screen edge. Everything else —
   obstacle sizes, jump arc, gaps — is unchanged, which makes the game a little
   more intense than in a browser window.
3. **Pixel polarity is flipped in the driver.** The ST7305 lights a pixel for a
   *set* bit (Waveshare's own U8g2 demo renders light-on-dark), whereas U8g2's
   framebuffer uses a set bit for ink. `rlcd.cpp` therefore transmits the
   reference lookup table with each row reversed. This also makes the
   night-mode buffer fill come out as a dark screen rather than a bright one.

The simulation runs in the original 600×150 canvas coordinate space on a fixed
60 Hz tick (Chromium's `FPS`), so the per-frame constants transfer unchanged;
only the final `y` is offset by `RENDER_DY` when drawing.

## Layout

```
dino_rlcd/
├── dino_rlcd.ino   setup/loop, 60 Hz fixed-timestep pacing, buttons, serial console
├── config.h        pin map, panel size, tick rate
├── audio.{h,cpp}   Chromium's synthesised sound cues -> ES8311
├── rlcd.{h,cpp}    ST7305 300x400 driver (U8g2 framebuffer + direct SPI panel writes)
├── game.{h,cpp}    the ported game
├── sprites.h       generated from Chromium's sprite sheet - do not edit
└── src/ExternLib/  vendored codec_board + esp_codec_dev (Waveshare / Espressif, MIT)
```

The ST7305 init sequence and the tile → panel-row conversion are ported from
Waveshare's reference driver in
[`waveshareteam/ESP32-S3-RLCD-4.2`](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2).
Sprites are blitted by hand rather than with `u8g2_DrawXBMP`, because obstacles
and the 600 px horizon tile need to scroll past the left edge and
`u8g2_DrawXBMP` rejects negative coordinates.

## Build and flash

Requires `arduino-cli` with `esp32:esp32` ≥ 3.3.0 and the **U8g2** library.
These commands are run from the top of the repository:

```sh
arduino-cli lib install U8g2

FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,\
FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600,DebugLevel=none"

arduino-cli compile --fqbn "$FQBN" dino_rlcd
arduino-cli upload  -p /dev/cu.usbmodem1401 --fqbn "$FQBN" dino_rlcd
```

These match Waveshare's recommended IDE settings for this board (USB CDC on
boot, OPI PSRAM, 16 MB flash).

**This firmware is not installed alongside the Game Boy emulator.** The Arduino
partition scheme above and the emulator's own `partitions.csv` disagree about
where the application lives, so flashing either one leaves the other unbootable.
Re-flashing is how you switch between them; see the
[top-level README](../README.md#one-board-one-firmware-at-a-time).

## Serial console

The firmware exposes a small diagnostic console at 115200 baud, which is handy
because the panel is the only other output. Status and framebuffer output run
on a low-priority background task through a bounded queue, so serial backpressure
does not block the 60 Hz game loop:

| Key | Action |
| --- | --- |
| `d` | Dump the framebuffer (`FBUF` magic + length + raw u8g2 buffer) so a host can rebuild a screenshot |
| `s` | Print one status line: state, score, high score, speed, dino y, nearest obstacle |
| `k` / `u` | Inject a KEY press / release |
| `b` | Inject a BOOT press |

A heartbeat line (`fps=…`) is printed every 2 seconds.

## Verified on hardware

* Steady **60 fps** with a full-screen flush every frame.
* Closed-loop autoplay over the serial console reached **score 1414** with one
  crash in 150 s, ~175 clean jumps — the jump arc clears the widest cactus
  clusters at every speed.
* Idle, running, jumping, paused, game-over, score flash and night-mode frames
  were all dumped from the panel and eyeballed.
* High score persists across reboots (NVS, namespace `dino`).
* Pixel polarity confirmed on the physical panel.
* With sound enabled, a 150 s autoplay reached **score 1464** with one crash and
  ~190 cues fired (176 jumps, 14 score milestones, 2 crashes) with no resets,
  still at 60 fps. The clincher that the audio task doesn't disturb the
  simulation: speed advanced 6.4 → 12.4 across 100 s, exactly the
  `ACCELERATION` × 60 Hz × 100 s the model predicts.
* The audio itself still needs an ear — attach a speaker to the MX1.25
  connector to hear the cues.
* A top-left battery icon shows the Waveshare ADC-derived 0–100% estimate.
  The bolt icon is inferred from sustained battery-voltage rise because the
  board exposes no documented digital charger-status input; it may not show
  while charging if the battery voltage is nearly flat.

## Notes

* The high score can be cleared by erasing NVS, e.g.
  `esptool.py --port /dev/cu.usbmodem1401 erase_region 0x9000 0x6000`.
* Sound needs a speaker on the MX1.25 2-pin connector. Without one the codec
  still initialises and the mixer runs silently, so nothing else changes.
* Opening the USB-CDC port with a terminal program asserts DTR/RTS and resets
  the ESP32-S3, and the port number can change when that happens.
