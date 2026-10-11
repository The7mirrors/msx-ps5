// MSX PS5 (2026-10-10, AI-assisted, from WHEN98): the USB keyboard and mouse, read by the helper payload and passed to the app.
//
// On firmware 12+ the app may not load libSceKeyboard or libSceMouse (0x80020063), and in a payload their init never
// returns. But the console's kernel is FreeBSD's, with its ugen driver: the helper (a payload, with root) finds the HID
// keyboards and mice under /dev/ugen*, reads their reports from the interrupt endpoints (/dev/usb/<bus>.<addr>.<ep>),
// and sends what is held and how far the mouse went to the app over 127.0.0.1:9089 (ProsperoKbdMouse.cpp).
// Only while the app is connected: the keyboard is taken from the console's own driver then (it ate the key reports
// otherwise) and handed back (re-enumerated) when the app goes.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace usbhid
{
constexpr uint16_t kPort = 9089;
constexpr uint32_t kMagic = 0x4b383957; // "W98K"

// helper -> app, each time something changed (and every second, so the app sees the helper is alive)
struct Packet
{
	uint32_t magic;
	uint8_t keyboard, mouse; // a keyboard / a mouse is plugged in
	uint8_t key_count;
	uint8_t buttons; // bit 0 left, 1 right, 2 middle
	int32_t dx, dy, wheel; // since the last packet
	uint16_t keys[24]; // HID usages held (the modifiers as 0xe0..0xe7)
};

// The helper side: a thread serving kPort (returns at once).
void StartServer();
} // namespace usbhid
