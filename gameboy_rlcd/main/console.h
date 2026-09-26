// Serial diagnostics console on the USB-Serial-JTAG port.
//
//   d  dump the framebuffer (FBUF <len> then hex rows) for a host to rebuild
//   s  one status line (state, whether a menu is up, Bluetooth, audio)
//   k  inject a KEY press      u  inject a KEY release
//   b  inject a BOOT press     n  release both
//   f  frame timing summary
//   F  time one full-screen repaint, split into repacking and SPI
//   x  frame-skip policy: auto, never, or every other frame
//   i  the pad's button mapping: each control, the index it uses, and the Game Boy button that comes out
//   j  inject a pad button press (cycles through the indices)
//   g  core registers
//   p  toggle logging of every BLE input report (for unknown pads)
//   B  start a BLE scan        l  list scan results     c  forget the bond
//   C  connect to the first scan result
#pragma once

#include <stdint.h>

// How unpainted frames are chosen when the core cannot fill the panel at 60 Hz.
#define FRAME_SKIP_AUTO 0
#define FRAME_SKIP_NEVER 1
#define FRAME_SKIP_ALTERNATE 2

void console_init(void);

// Non-blocking; call once per emulated frame.
void console_poll(void);

// Current frame-skip policy, set with 'x'.
int console_frame_skip_mode(void);

// Record a frame's timings for the 'f' report.
void console_frame(int64_t total_us, int64_t emu_us, int64_t flush_us, int64_t audio_us, int g0,
                   int g1);
