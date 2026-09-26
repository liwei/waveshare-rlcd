// Menus, notices and the ROM picker, drawn on the shared landscape
// framebuffer. Text is rendered at 2x so it matches the Game Boy's scale.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Menu rows are 16 px tall; rows 0..17 fill the panel, and the status strip
// occupies the bottom 12 px.
#define UI_ROW_H 16
#define UI_MENU_ROWS 18
#define UI_TITLE_ROW 0
#define UI_SCROLL_UP_ROW 1
#define UI_LIST_FIRST 2
#define UI_LIST_LAST 15
#define UI_SCROLL_DOWN_ROW 16
#define UI_HINT_ROW 17
#define UI_LIST_VISIBLE (UI_LIST_LAST - UI_LIST_FIRST + 1)

void ui_clear(void);
void ui_flush(void);

// Fill a menu row with bg_ink and draw text into it, 1 px in from the left.
void ui_row(int row, const char *text, bool text_ink, bool bg_ink);

void ui_text(int x, int y, const char *s, int scale, bool text_ink, bool bg_ink);

// Bottom status strip: Bluetooth state on the left, battery on the right.
void ui_status_bar(void);

// Draw a message and hold it for duration_ms (0 = return immediately).
void ui_notice(const char *title, const char *message, uint32_t duration_ms);

// Vertical menu over the whole panel. Returns the chosen index, or -1 if the
// user backed out. `badge` is called per item to draw its right-hand status, or
// may be NULL.
typedef void (*ui_badge_fn)(int index, char *out, size_t out_size);

int ui_menu(const char *title, const char *const *items, int count, int initial,
            ui_badge_fn badge);

// True while a menu owns the screen.
bool ui_menu_active(void);

// Scans mount_pt (and one level of subdirectories) for .gb/.gbc files and lets
// the user choose one.
typedef enum {
  UI_ROM_PICK_NONE = 0,
  UI_ROM_PICK_SELECTED,
  UI_ROM_PICK_LOAD_LAST,
  UI_ROM_PICK_MANAGER, /* the WiFi ROM manager, not a game */
} ui_rom_pick_result_t;

ui_rom_pick_result_t ui_rom_picker(const char *mount_pt, char *out_path, size_t path_size,
                                   const char *last_rom);

// How many ROMs were found by the last picker run, and the total found so far.
int ui_rom_count(void);
const char *ui_rom_name(int index);
