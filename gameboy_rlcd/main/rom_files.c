#include "rom_files.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "dirent.h"
#include "esp_vfs_fat.h"

#include "config.h"

static bool has_rom_extension(const char *name) {
  const char *dot = strrchr(name, '.');
  if (dot == NULL) {
    return false;
  }

  return strcasecmp(dot, ".gb") == 0 || strcasecmp(dot, ".gbc") == 0;
}

// Replaces the extension of a relative name, e.g. "roms/x.gbc" -> "roms/x.sav".
static void sibling(const char *name, const char *ext, char *out, size_t out_size) {
  const char *slash = strrchr(name, '/');
  const char *dot = strrchr(name, '.');
  if (dot == NULL || (slash != NULL && dot < slash)) {
    dot = name + strlen(name);
  }

  const size_t base = (size_t)(dot - name);
  if (base + strlen(ext) + 1 > out_size) {
    out[0] = '\0';
    return;
  }

  memcpy(out, name, base);
  memcpy(out + base, ext, strlen(ext) + 1);
}

static bool exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static uint64_t file_size(const char *path) {
  struct stat st;
  return (stat(path, &st) == 0) ? (uint64_t)st.st_size : 0;
}

static int add_entry(rom_entry_t *out, int count, int max, const char *relative) {
  if (count >= max || !has_rom_extension(relative)) {
    return count;
  }

  // Two path buffers, not five: this runs on the web server's task, whose stack
  // is internal RAM this build does not have to spare.
  char path[ROM_FILES_PATH_MAX];
  char sibling_path[ROM_FILES_PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, relative);

  bool save = false;
  static const char *const kExtras[] = {".sav", ".state"};
  for (size_t i = 0; i < sizeof(kExtras) / sizeof(kExtras[0]) && !save; i++) {
    char name[ROM_FILES_REL_MAX];
    sibling(relative, kExtras[i], name, sizeof(name));
    if (name[0] == '\0') {
      continue;
    }

    snprintf(sibling_path, sizeof(sibling_path), "%s/%s", SD_MOUNT_POINT, name);
    save = exists(sibling_path);
  }

  rom_entry_t *entry = &out[count];
  strlcpy(entry->name, relative, sizeof(entry->name));
  entry->size = file_size(path);
  entry->save = save;
  return count + 1;
}

int rom_files_list(rom_entry_t *out, int max) {
  DIR *dir = opendir(SD_MOUNT_POINT);
  if (dir == NULL) {
    return 0;
  }

  int count = 0;
  struct dirent *ent;

  // The top level first, so a mixed card reads in a predictable order.
  while ((ent = readdir(dir)) != NULL && count < max) {
    if (ent->d_name[0] == '.') {
      continue;
    }

    char path[ROM_FILES_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, ent->d_name);

    struct stat st;
    const bool is_dir = (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
    if (!is_dir) {
      count = add_entry(out, count, max, ent->d_name);
    }
  }
  closedir(dir);

  // Then one directory down, matching the picker.
  dir = opendir(SD_MOUNT_POINT);
  if (dir == NULL) {
    return count;
  }

  while ((ent = readdir(dir)) != NULL && count < max) {
    if (ent->d_name[0] == '.') {
      continue;
    }

    char sub[ROM_FILES_PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/%s", SD_MOUNT_POINT, ent->d_name);

    struct stat st;
    if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) {
      continue;
    }

    DIR *inner = opendir(sub);
    if (inner == NULL) {
      continue;
    }

    struct dirent *child;
    while ((child = readdir(inner)) != NULL && count < max) {
      if (child->d_name[0] == '.') {
        continue;
      }

      char relative[ROM_FILES_REL_MAX];
      snprintf(relative, sizeof(relative), "%s/%s", ent->d_name, child->d_name);
      count = add_entry(out, count, max, relative);
    }
    closedir(inner);
  }
  closedir(dir);

  return count;
}

void rom_files_space(uint64_t *total, uint64_t *free_bytes) {
  uint64_t t = 0;
  uint64_t f = 0;

  if (esp_vfs_fat_info(SD_MOUNT_POINT, &t, &f) != ESP_OK) {
    t = 0;
    f = 0;
  }

  if (total != NULL) {
    *total = t;
  }
  if (free_bytes != NULL) {
    *free_bytes = f;
  }
}

bool rom_files_valid_upload(const char *name) {
  if (name == NULL || name[0] == '\0' || name[0] == '.') {
    return false;
  }
  if (strlen(name) >= ROM_FILES_NAME_MAX) {
    return false;
  }
  if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
    return false;
  }

  return has_rom_extension(name);
}

bool rom_files_delete(const char *name) {
  if (name == NULL || name[0] == '\0' || name[0] == '.') {
    return false;
  }
  // Only something the listing could have produced. A path that climbs out, or
  // that is not a ROM, is not something this API has any business touching.
  if (strstr(name, "..") != NULL || strchr(name, '\\') != NULL || !has_rom_extension(name)) {
    return false;
  }
  const char *slash = strchr(name, '/');
  if (slash != NULL && strchr(slash + 1, '/') != NULL) {
    return false;
  }

  char path[ROM_FILES_PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, name);
  if (remove(path) != 0) {
    return false;
  }

  static const char *const kExtras[] = {".sav", ".state"};
  for (size_t i = 0; i < sizeof(kExtras) / sizeof(kExtras[0]); i++) {
    char sibling_name[ROM_FILES_REL_MAX];
    sibling(name, kExtras[i], sibling_name, sizeof(sibling_name));

    char sibling_path[ROM_FILES_PATH_MAX];
    snprintf(sibling_path, sizeof(sibling_path), "%s/%s", SD_MOUNT_POINT, sibling_name);
    remove(sibling_path); // absence is not an error
  }

  return true;
}

const char *rom_files_mount(void) { return SD_MOUNT_POINT; }
