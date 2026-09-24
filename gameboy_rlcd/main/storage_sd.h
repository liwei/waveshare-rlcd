// TF card on the ESP32-S3 SDMMC host, 1-bit bus, mounted at /sdcard.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool storage_sd_mount(void);
bool storage_sd_ready(void);
const char *storage_sd_error(void);

// Reads a whole file. Prefers internal RAM so the emulator can fetch ROM bytes
// quickly, falling back to PSRAM for large ROMs. Free with storage_free().
uint8_t *storage_sd_read_file(const char *path, size_t *out_size);

void storage_free(void *p);
