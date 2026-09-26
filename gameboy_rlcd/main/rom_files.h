// The SD card seen as a set of ROMs, for the web manager.
//
// Deliberately separate from the network code: this is the part that touches the
// filesystem, and it follows the same rules the ROM picker does (top level and
// one directory down, .gb/.gbc, no dotfiles) so the page and the picker always
// agree about what is on the card.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ROM_FILES_MAX 96
// One path component, matching the FATFS long-name limit; a relative path of up
// to two components plus an extension ("roms/name.gbc"); and a full path with
// the mount point. Kept as separate sizes so the compiler can see that every
// snprintf below fits.
#define ROM_FILES_NAME_MAX 256
#define ROM_FILES_REL_MAX (2 * ROM_FILES_NAME_MAX + 8)
#define ROM_FILES_PATH_MAX (sizeof("/sdcard") + ROM_FILES_REL_MAX + 2)

typedef struct {
  char name[ROM_FILES_NAME_MAX]; /* relative to the mount, e.g. "roms/foo.gbc" */
  uint64_t size;
  bool save; /* a .sav or .state sits alongside it */
} rom_entry_t;

// Fills `out` with up to `max` ROMs and returns how many. Zero when the card is
// not mounted.
int rom_files_list(rom_entry_t *out, int max);

// Card capacity in bytes. Zeros when the card is not mounted.
void rom_files_space(uint64_t *total, uint64_t *free_bytes);

// Whether a name is acceptable to create: a plain file name, no directory parts,
// not hidden, and a ROM extension. This is what stops an upload from writing
// somewhere unexpected.
bool rom_files_valid_upload(const char *name);

// Whether a name may be deleted: anything the listing could have produced,
// which is a ROM with an optional single directory part. A ROM's save files go
// with it.
bool rom_files_delete(const char *name);

// The card's mount point, for callers that need to build a path.
const char *rom_files_mount(void);
