// WiFi ROM manager: the device's own access point and the page that manages the
// SD card's ROMs.
//
// Everything network-related lives here, so the rest of the firmware never has
// to include esp_wifi. The access point exists only while this mode is on: WiFi
// and Bluetooth share the radio, and the emulator has a frame budget to keep.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Brings up the access point and the HTTP server. False if either fails, with
// nothing left running.
bool web_start(void);

// Stops the server and the access point, leaving the WiFi stack initialised so
// the next web_start() is quick.
void web_stop(void);

bool web_running(void);

// For the screen: how many devices have joined, and the address to browse to.
int web_client_count(void);
const char *web_ip(void);

// The credentials, so the screen and the README cannot disagree.
const char *web_ssid(void);
const char *web_password(void);
