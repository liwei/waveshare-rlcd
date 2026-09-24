// Chromium dino game, ported to the ESP32-S3-RLCD-4.2.
//
// Gameplay values and state machine are copied verbatim from Chromium's
// components/neterror/resources/dino_game/ (see game.cpp for per-constant
// citations). The world is simulated in the original 600x150 canvas space and
// translated to the 400x300 panel at draw time.
#pragma once

#include "rlcd.h"

void Game_Init();
void Game_Tick();            // one 60 Hz simulation step
void Game_Draw(Rlcd &lcd);   // render the current state
void Game_KeyDown();         // KEY  pressed
void Game_KeyUp();           // KEY  released
void Game_KeyTogglePause();  // BOOT pressed

// One-line summary of the current state, for the serial heartbeat.
const char *Game_StatusLine();
