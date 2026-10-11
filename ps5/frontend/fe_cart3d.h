// MSX PS5 frontend: games on the shelf as real 3D objects - the game's box (its cover on the front, a spine and a back
// made from its art), and the cartridge (or 3.5" floppy, for disk games) that comes out of it when it is played.
//
// A small software renderer: rounded, bevelled bodies; printed faces (the art) and plastic, metal and laminated
// paper lit like a studio product shot (key, fill and rim light); 2x2 samples a pixel; a reflection and a soft
// shadow on the floor. The surface is A8B8G8R8 (ps5video); drawing is split in screen columns so the shelf's thread
// pool can share a frame.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace cart3d
{
enum class Kind
{
	Box,       // the game's box (portrait art on the front)
	BoxWide,   // a landscape box (landscape art on the front)
	Cartridge, // an MSX ROM cartridge (landscape label)
	Floppy     // a 3.5" disk (square-ish label)
};

// A picture for a face: row-major A8B8G8R8 with smaller levels (each half the size), for distant games.
struct Texture
{
	struct Level
	{
		int w = 0, h = 0;
		std::vector<uint32_t> px;
	};
	std::vector<Level> levels;
	uint32_t average = 0xff303030u; // A8B8G8R8: the plain faces (top, bottom) and the glow
	static std::shared_ptr<Texture> FromPixels(const uint32_t* abgr, int w, int h);
};
using TexPtr = std::shared_ptr<const Texture>;

// The art of one game, for its box and its cartridge / floppy.
struct Art
{
	TexPtr front; // the cover (box front)
	TexPtr spine; // the box's sides
	TexPtr back;  // the box's back
	TexPtr label; // the cartridge's / floppy's label
	TexPtr disk_label; // a floppy's label (every game has one: the shelf of floppies); null: `label`
	bool konami = false; // Konami's cartridge (a strip with two holes on the left, a boss on the right end); else generic
};

// Where and how one object stands. World units: the eye is at the origin looking down +z, y up; the floor is y = 0.
struct Pose
{
	float x = 0, z = 1000; // the centre of its base on the floor
	float lift = 0;        // raised above the floor (negative: going down into the slot)
	float yaw = 0;         // radians, turning around the vertical axis (positive: the right side comes forward)
	float pitch = 0;       // radians, tilting back around its base
	float height = 540;    // its height in world units
	float bright = 1;      // 0..1 dimming for games away from the middle
	float alpha = 1;       // 0..1 fade
	bool reflection = true;
	bool shadow = true;
	float clip_floor = -1; // >= 0: nothing below this height is drawn (the slot swallows it)
	float shutter = 0;     // a floppy's metal shutter: 0 closed .. 1 slid open (the disk shows)
	bool fast = false;     // moving: one sample a pixel (no smoothed edges)
};

struct View
{
	int w = 1920, h = 1080;
	float focal = 1000;  // pixels at distance 1
	float horizon = 455; // screen row of the eye
	float eye_y = 227;   // eye height above the floor
};

// The body's width / height / depth over its height.
void Proportions(Kind k, float& w, float& d);
// Screen columns the object covers; false when it is off screen.
bool Extent(const View& v, Kind k, const Pose& p, int& x0, int& x1);
// Draws columns [x0, x1) of one object: its shadow and reflection, then the body. Draw far objects first.
void Draw(uint32_t* surface, const View& v, Kind k, const Pose& p, const Art& art, int x0, int x1);

// An object drawn once and put on screen many times (a game at the side of the shelf only slides sideways): its
// pixels say how much of the screen under them stays (keep, 0..255) and what is added (colour, A8B8G8R8 without
// alpha) - its shadow, its reflection and its body together. (x0, y0): its top left on screen as drawn.
struct Sprite
{
	int x0 = 0, y0 = 0, w = 0, h = 0;
	std::vector<uint32_t> color;
	std::vector<uint8_t> keep;
};
// Sizes and clears s for the object at p; false when it is not in view.
bool Prepare(Sprite& s, const View& v, Kind k, const Pose& p);
// Draws columns [x0, x1) (screen columns, may lie outside the screen) of the object into s, after Prepare.
void Render(Sprite& s, const View& v, Kind k, const Pose& p, const Art& art, int x0, int x1);
// Puts s on the surface moved dx pixels to the right, its colours times bright (0..), faded by alpha; columns [x0, x1).
void Blit(uint32_t* surface, const View& v, const Sprite& s, int dx, float bright, float alpha, int x0, int x1);
// The mesh's triangle edges (to look at the model): facing ones in `front`, the hidden ones in `back` (0: not drawn).
// Returns the triangle count.
int Wireframe(uint32_t* surface, const View& v, Kind k, const Pose& p, uint32_t front, uint32_t back);
} // namespace cart3d
