// MSX PS5 frontend: a game's 3D art (fe_cart3d.h) made from its cover - the box's front (the cover), its spine (the
// title running up it, the MSX badge), its back (the cover darkened, the title), and the cartridge's / floppy's label
// (Konami's style for Konami's games: a cream title band, the art in a frame, an orange band with the badge; a
// generic label for the rest).
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_cart3d.h"
#include "fe_games.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace fe
{
// A picture for the box's back (a decoded PNG, row-major A8B8G8R8).
struct Shot
{
	std::vector<uint32_t> px;
	int w = 0, h = 0;
};
// The game's screens: in-game pictures (up to three) and its title screen (w 0: none).
struct Shots
{
	std::vector<Shot> snaps;
	Shot title;
};
// rgba: the cover, row-major A8B8G8R8, w x h; shots: its screens for the back (or none).
// box_px (box_w x box_h): the picture for the box (portrait); null: the cover.
std::shared_ptr<const cart3d::Art> MakeArt(const uint32_t* rgba, int w, int h, const GameInfo& g, const Shots* shots = nullptr,
	const uint32_t* box_px = nullptr, int box_w = 0, int box_h = 0);
// The game's object on the shelf when played: a floppy for disk images, else a cartridge.
cart3d::Kind MediaOf(const GameInfo& g);
// A Konami game (by its title): Konami's cartridge and label.
bool IsKonami(const GameInfo& g);
// 2026-10-16: an Arabic MSX cartridge ("<Title> (Al Alamiah)", "(Barq)", "(Al-Mithali)"): its own label, an Arabic MSX
bool IsArabicEdition(const GameInfo& g);
// one of Al Alamiah's eight game cartridges: Al Alamiah's cartridge shape
bool IsAlamiah(const GameInfo& g);
} // namespace fe
