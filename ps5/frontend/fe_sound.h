// MSX PS5 (2026-10-11, AI-assisted): the shelf's and the menus' sound - effects (CC0 recordings and Kenney's UI
// sounds, WAVs in /data/msx/sounds) and the background music, made live by a small synth in the style of the MSX's
// sound chip (an original tune; nothing to load). Mixed into the PS5 audio ring while no game runs.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace fe
{
namespace sound
{
enum class Sfx
{
	Browse,       // left / right on the shelf
	MenuMove,     // up / down in a menu
	MenuOk,       // a setting changes / a choice
	MenuBack,     // back
	Open,         // a menu opens
	Close,        // a menu closes
	Favorite,     // a favourite added / removed
	CartClack,    // the cartridge goes into the slot
	FloppyInsert, // the floppy slides into the drive, the latch
	FloppySeek,   // the drive's head steps
	ShutterSlide, // the floppy's metal shutter slides open
	ShutterSnap,  // ... and springs shut
	Count
};

void Init();                       // loads the effects (once); harmless if they are missing
void Play(Sfx s, float volume = 1); // an effect (if effects are on)
void Music(bool on);               // the background music: fades in / out (if music is on in the settings)
void StopAll();                    // a game starts: silence at once
void Pump();                       // once a frame while no game runs: keeps the audio ring filled

// The music alone, for a preview on the PC: `frames` stereo frames at 48 kHz from the start of the loop.
void RenderMusic(int16_t* stereo, int frames);
} // namespace sound
} // namespace fe
