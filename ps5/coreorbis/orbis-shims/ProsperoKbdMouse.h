// MSX PS5 (2026-10-10, AI-assisted, from WHEN98): a real USB keyboard and mouse on the PS5, for the MSX.
//
// The helper payload reads them from USB (ProsperoUsbHid.h) and sends their state over a local socket; a thread here
// receives it, and the frontend takes the keys held and the mouse's movement once per frame.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace ps5kbm
{
// Starts the reading thread (once).
void Start();

struct Snapshot
{
	bool keyboard = false, mouse = false; // a keyboard / a mouse is connected
	uint16_t keys[24] = {}; // HID usages held (the modifiers as 0xe0..0xe7)
	int key_count = 0;
	int dx = 0, dy = 0, wheel = 0; // the mouse's movement and wheel notches since the last Take
	uint32_t buttons = 0; // bit 0 left, 1 right, 2 middle
};
// What is held now, and the mouse movement since the last call.
Snapshot Take();
} // namespace ps5kbm
