# ESP32-S3-RLCD-4.2 projects

Two firmwares for one board: a **Game Boy emulator** and **Chrome's offline
dino game**, both drawn on the same 4.2" reflective LCD.

| Project | What it is | Built with |
| --- | --- | --- |
| [`gameboy_rlcd/`](gameboy_rlcd/README.md) | A Game Boy that runs from ROMs on the SD card, with a Bluetooth gamepad, save states and a WiFi ROM manager. | ESP-IDF 5.5 |
| [`dino_rlcd/`](dino_rlcd/README.md) | The T-Rex runner, ported from Chromium's own artwork and constants. | arduino-cli |

Neither uses a backlight or colour: the panel is reflective and one bit per
pixel, so everything is drawn in four ordered-dither shades of ink on paper. The
Game Boy emulator is the larger of the two and the one still being worked on;
the dino port is finished, and both are documented and measured on real
hardware rather than on a simulator.

## The board

Waveshare ESP32-S3-RLCD-4.2 — an ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB OPI
PSRAM, **Bluetooth 5 LE only, no classic Bluetooth**) with a 300×400 ST7305
panel, a microSD slot, an ES8311 codec and two buttons.

| Function | Pins |
| --- | --- |
| RLCD (SPI2) | SCLK 11, MOSI 12, CS 40, DC 5, RST 41 |
| TF card (SDMMC, 1-bit) | CLK 38, CMD 21, D0 39 |
| ES8311 codec | I2S MCLK 16, BCLK 9, WS 45, DOUT 10; I2C SDA 13, SCL 14; PA 46 |
| Buttons | KEY = GPIO18, BOOT = GPIO0, active low |
| Battery | ADC1 channel 3 = GPIO4, behind a 100k/200k divider |

Sound needs a speaker on the MX1.25 2-pin connector; without one the codec still
initialises and runs silently. The panel's driver came from Waveshare's own
[`ESP32-S3-RLCD-4.2`](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2)
reference, as did the pin map above.

## One board, one firmware at a time

**Flashing one of these replaces the other.** They need different partition
tables: the dino game is an Arduino sketch built with `PartitionScheme=app3M_fat9M_16MB`,
while the emulator defines its own `partitions.csv` — two 2 MB application slots
plus `otadata`, so it can carry the ROM manager in the second slot and switch
between them. The two layouts put the application in different places, so the
second one flashed is the one that boots, and switching means re-flashing.

The SD card is untouched by all of this. ROMs, saves and the emulator's config
live on the card, not in flash, so they survive re-flashing either firmware.

## Layout

```
.
├── gameboy_rlcd/         the emulator: ESP-IDF project
│   ├── main/             front end, menus, panel driver, input, audio, console
│   ├── manager/          the WiFi ROM manager, a second application
│   ├── components/       vendored codec_board + esp_codec_dev
│   ├── tools/            flash both apps, CJK font generator, WiFi QR, BLE scan
│   └── test/             host-side test for the HID report descriptor parser
└── dino_rlcd/            the dino game: Arduino sketch
    ├── dino_rlcd.ino     setup/loop, 60 Hz pacing, buttons, serial console
    └── src/ExternLib/    vendored codec_board + esp_codec_dev
```

Each project vendors its own copy of Waveshare's `codec_board` and Espressif's
`esp_codec_dev`, adapted to its own build system — an Arduino sketch and an
ESP-IDF component do not want the same build files. There is no shared source
between the two.

## Building

The two use different toolchains and neither build touches the other.

**The emulator** needs ESP-IDF 5.5 and the Xtensa toolchain:

```sh
. ~/esp/esp-idf/export.sh
cd gameboy_rlcd
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Both of its applications are built and flashed by `gameboy_rlcd/tools/flash_all.sh`,
which is what you want unless you enjoy re-flashing the emulator by hand after
the manager's own `idf.py flash` has overwritten it.

**The dino game** needs `arduino-cli` with `esp32:esp32` ≥ 3.3.0 and the U8g2
library:

```sh
arduino-cli lib install U8g2
arduino-cli compile --fqbn "$FQBN" dino_rlcd
arduino-cli upload  -p /dev/cu.usbmodem1401 --fqbn "$FQBN" dino_rlcd
```

`$FQBN` is spelled out in [`dino_rlcd/README.md`](dino_rlcd/README.md#build-and-flash).

Both are flashed over the board's native USB-C port, which is also the serial
console for each. Both open at 115200 baud.

## Provenance

* **Game Boy core** — [PaperBoy](https://gitlab.com/zephray/paperboy)'s build of
  Peanut-GB, vendored unmodified, with PaperBoy's menus adapted to this panel.
  Peanut-GB © Mahyar Koshkouei, MIT; PaperBoy © Wenting Zhang. Pinned at
  `e70b293`.
* **Dino artwork and gameplay** — Chromium's, extracted pixel-for-pixel from its
  sprite sheet and ported rule by rule from its TypeScript. Nothing is redrawn.
* **Panel driver and codec glue** — ported from Waveshare's reference driver and
  their `codec_board`, MIT.

Two things the emulator does not do, in case they are what you came for: **CGB
colour is not emulated** (the hardware is, the palettes are not, so colour games
are drawn as four shades), and there is no link cable. The
[emulator's README](gameboy_rlcd/README.md#not-implemented) has the details.
