// MSX PS5 frontend: the Genesis Plus GX core behind a small API.
//
// The core is driven through its libretro interface (libretro/libretro.c, the most complete of its ports): the
// frontend is the libretro "frontend" -- it answers the core's environment calls (folders, options, log), gets
// each frame (RGB565) and its sound (44.1 kHz) through callbacks, and reports the pads. One retro_run() is one
// frame, called from the frontend's thread, as Snes9x PS5 runs Snes9x.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace emu
{
// The choices the settings menu offers.
struct Choice
{
	const char* name;
	const char* value; // the core option's value ("" where the frontend handles it)
};
extern const Choice kAspects[]; // the core's TV aspect, square pixels, 4:3, 16:9
extern const int kAspectCount;
// MSX PS5: the Genesis names kept, the meanings blueMSX's (fe_menu.cpp labels them)
extern const Choice kNtscFilters[]; // bluemsx_vdp_synctype: auto, 50 Hz, 60 Hz
extern const int kNtscFilterCount;
extern const Choice kBorders[]; // bluemsx_overscan: show the borders, crop them, crop (MSX2)
extern const int kBorderCount;
extern const Choice kFmChips[]; // bluemsx_ym2413_enable: the FM-PAC / MSX-MUSIC sound chip on or off
extern const int kFmChipCount;
extern const Choice kRegions[]; // bluemsx_msxtype: the machine (auto = MSX2+, MSX, MSX2, MSX2+, turbo R)
extern const int kRegionCount;
extern const Choice kPadTypes[]; // what the D-pad drives: the joystick and the cursor keys, the joystick, the keys
extern const int kPadTypeCount;
extern const Choice kMultitaps[]; // (unused)
extern const int kMultitapCount;

// The buttons of the console's pad the player can move, and the PS5 buttons they can go to.
struct PadButton
{
	const char* name; // "Square"
	unsigned bit; // SCE_PAD_BUTTON_*; 0 = not assigned
};
extern const PadButton kPs5Buttons[]; // Cross, Circle, Square, Triangle, L1, R1, OPTIONS, touchpad, (none)
extern const int kPs5ButtonCount;
// An MSX joystick button or keyboard key the player can put on a PS5 button.
struct ConsoleButton
{
	const char* name; // "Joystick button 1", "Space", "F1"...
	const char* key; // its key in msx-ps5.ini: "btn_space"
	unsigned retro_id; // the RetroPad button blueMSX reads for it (kNoPad for a keyboard key)
	unsigned retrok; // the keyboard key (RETROK_*) blueMSX reads for it, 0 for a joystick button
	int default_ps5; // index in kPs5Buttons
};
constexpr unsigned kNoPad = 255;
constexpr int kConsoleButtonCount = 18;
extern const ConsoleButton kConsoleButtons[kConsoleButtonCount];

bool InitCore();
void DeinitCore();

// Starts the game. On failure, *error says why (a missing Sega CD BIOS names the files and where they go).
bool LoadGame(const std::string& path, std::string* error);
void CloseGame(); // writes the battery save and ends the game
bool GameLoaded();
std::string GameName(); // the file name without its extension
std::string SystemName(); // "Mega Drive", "Sega CD"... of the running game

// Settings from fe::Config() -> the core's options and the frontend's picture/sound settings.
void ApplySettings();

enum class FrameResult
{
	Continue,
	OpenMenu, // L3 + R3 pressed together
	Stopped, // no game
};
// One frame: the pads and hot keys, retro_run(), the picture (and the messages) on the screen, the sound to
// the audio ring, paced by the display (60 Hz games) or by the sound (50 Hz games). Hot keys: L2 + Up / Down =
// save / load the state slot, L2 + Left / Right = change the slot, R2 held = fast forward, L2 + R2 held =
// rewind.
FrameResult RunFrame();

void Pause(); // nothing runs between frames; kept for the menus' symmetry
void Resume();

bool SaveState(int slot); // 1..10, /data/msx/states/<game>.state<slot>
bool LoadState(int slot);
bool StateExists(int slot);
void Reset();
void PowerCycle(); // the game loaded again, as switching the console off and on
void Osd(const std::string& text); // a message over the game for a few seconds

// Redraws the last frame into the surface (the pause menu draws over it).
void RedrawLastFrame();
} // namespace emu
