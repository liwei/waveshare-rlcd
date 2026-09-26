#!/bin/sh
# Builds and flashes both applications.
#
# The emulator goes into ota_0 through idf.py, which also writes the bootloader,
# the partition table and the initial otadata. The ROM manager is a separate
# project and its binary has to be written into ota_1 by hand, because its own
# idf.py would otherwise put it in ota_0 and overwrite the emulator.
#
# The offset below is ota_1's from partitions.csv. If the table changes, change it.
set -e

PORT=${1:-/dev/cu.usbmodem1401}
HERE=$(cd "$(dirname "$0")" && pwd)
OTA_1_OFFSET=0x220000

if [ -z "$IDF_PATH" ]; then
  echo "ESP-IDF is not exported: run '. ~/esp/esp-idf/export.sh' first." >&2
  exit 1
fi

cd "$HERE/.."

echo "== emulator -> ota_0 =="
idf.py build
if [ "$2" = "--erase" ]; then
  # Only needed when the partition table changes, and it clears the saved pad.
  idf.py -p "$PORT" erase-flash
fi
idf.py -p "$PORT" flash

echo "== rom manager -> ota_1 =="
cd manager
idf.py build
esptool.py --chip esp32s3 -p "$PORT" -b 460800 --before default_reset --after hard_reset \
  write_flash "$OTA_1_OFFSET" build/gameboy_manager.bin

echo "done"
