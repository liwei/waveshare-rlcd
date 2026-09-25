// Game Boy core wrapper: vendored from PaperBoy (zephray/paperboy @ e70b293).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GB_LCD_WIDTH 160
#define GB_LCD_HEIGHT 144

#define GB_BTN_A 0x01
#define GB_BTN_B 0x02
#define GB_BTN_SELECT 0x04
#define GB_BTN_START 0x08
#define GB_BTN_RIGHT 0x10
#define GB_BTN_LEFT 0x20
#define GB_BTN_UP 0x40
#define GB_BTN_DOWN 0x80

bool paperboy_gb_init(const uint8_t *rom, size_t rom_size);

// Selects the video mode used by the next paperboy_gb_init(): true runs a CGB
// cartridge on the colour core, false runs it as a monochrome DMG. Plain DMG
// cartridges ignore this. Changing it takes effect on re-initialisation.
void paperboy_gb_set_cgb_mode(bool enable);

// True when the machine is actually running as a CGB; a restored save state
// can change this mid-session.
bool paperboy_gb_is_cgb(void);

void paperboy_gb_set_buttons(uint8_t pressed_mask);

// The buttons the core is currently being fed, after the menu chord has been
// consumed. For diagnostics.
uint8_t paperboy_gb_buttons(void);

// Run one Game Boy frame, blitting changed scanlines into the shared
// framebuffer. dirty_g0/dirty_g1 receive the panel column-group range that
// changed, or g1 < g0 when nothing did.
bool paperboy_gb_run_frame(bool skip_render, int *dirty_g0, int *dirty_g1);

// Make the next frame redraw every scanline (after a menu has drawn over it).
void paperboy_gb_invalidate_video(void);

// One line of core registers, for the serial console.
void paperboy_gb_dump_state(char *out, size_t out_size);

bool paperboy_gb_has_persist(void);
size_t paperboy_gb_persist_size(void);
bool paperboy_gb_persist_is_dirty(void);
bool paperboy_gb_persist_export(uint8_t *dst, size_t dst_size, uint32_t timestamp);
bool paperboy_gb_persist_import(const uint8_t *src, size_t src_size, uint32_t current_timestamp);
void paperboy_gb_persist_mark_clean(void);
size_t paperboy_gb_state_size(void);
bool paperboy_gb_state_export(uint8_t *dst, size_t dst_size);
bool paperboy_gb_state_import(const uint8_t *src, size_t src_size);
bool paperboy_gb_is_ready(void);
const char *paperboy_gb_last_error(void);
