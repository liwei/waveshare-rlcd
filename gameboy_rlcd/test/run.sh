#!/bin/sh
# Host-side test for the HID report descriptor parser. Builds hid_gamepad.c
# against a stub of the ESP-IDF logging macros and checks it against a real
# 139-byte gamepad descriptor plus a conventional hat-switch descriptor.
#
#   sh gameboy_rlcd/test/run.sh
set -e
here=$(dirname "$0")
cc -std=gnu17 -Wall -I"$here" -I"$here/../main" \
   "$here/hid_gamepad_test.c" "$here/../main/hid_gamepad.c" \
   -o "$here/hid_gamepad_test"
"$here/hid_gamepad_test"
