// MSX PS5: DualSense / DualShock input through libScePad (up to 4 players).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace ps5input
{
constexpr int kMaxPads = 4;

struct PadState
{
	bool connected = false;
	uint32_t buttons = 0; // SCE_PAD_BUTTON_*; the left stick also sets the D-pad bits
	uint32_t raw_buttons = 0; // without the stick
	uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
	uint8_t fingers = 0; // on the touchpad; the first one's position (about 0..1919, 0..1079)
	uint16_t tx = 0, ty = 0;
};

bool Init();
void Shutdown();

// Reads every pad (thread-safe). Player 1 is the foreground user; the other logged-in users follow in the system's
// order. Who is logged in is checked again every couple of seconds.
void Poll();
const PadState& Pad(int player); // the polling thread's view
PadState Snapshot(int player); // a copy, safe from any thread (Poll may run on another one)
int ConnectedCount();
// The user a player's pad belongs to (-1: none) - for their microphone (ProsperoMic.h).
int32_t UserId(int player);

// Player 1's buttons that went down in the last Poll (menus).
uint32_t Pressed();

// Player 1's controller: the motors (0..255 each; 0, 0 stops) - repeated values are not sent again ...
void Rumble(uint8_t large, uint8_t small);
// ... and L2 / R2's resistance, growing along the press (zones 0..9, strengths 1..8); TriggersOff frees them.
void TriggerSlope(uint8_t start_zone, uint8_t end_zone, uint8_t start_strength, uint8_t end_strength);
void TriggersOff();
// L2 / R2 buzz (the triggers' own motors): from zone `position` (0: at rest) on, amplitude 1..8, frequency in Hz.
void TriggerVibration(uint8_t position, uint8_t amplitude, uint8_t frequency);
} // namespace ps5input
