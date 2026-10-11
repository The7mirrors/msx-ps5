// MSX PS5 frontend: a game's 3D art made from its cover (fe_art.h).
//
// SPDX-License-Identifier: MIT
#include "fe_art.h"

#include "fe_text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "msx_logo.inc"
#include "msx_facts.inc"

namespace fe
{
namespace
{
uint32_t Px(int r, int g, int b)
{
	return 0xff000000u | uint32_t(std::clamp(b, 0, 255)) << 16 | uint32_t(std::clamp(g, 0, 255)) << 8 | uint32_t(std::clamp(r, 0, 255));
}
int R(uint32_t c) { return int(c & 0xff); }
int G(uint32_t c) { return int((c >> 8) & 0xff); }
int B(uint32_t c) { return int((c >> 16) & 0xff); }

struct Img
{
	int w = 0, h = 0;
	std::vector<uint32_t> px;
	Img() = default;
	Img(int ww, int hh, uint32_t fill = 0xff000000u) : w(ww), h(hh), px(size_t(ww) * hh, fill) {}
	uint32_t& at(int x, int y) { return px[size_t(y) * w + x]; }
	Canvas canvas() { return Canvas{px.data(), w, h, w}; }
};

struct Src
{
	const uint32_t* px;
	int w, h;
	uint32_t at(float x, float y) const // bilinear
	{
		x = std::clamp(x, 0.0f, float(w - 1)), y = std::clamp(y, 0.0f, float(h - 1));
		const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
		const float fx = x - x0, fy = y - y0;
		const uint32_t a = px[size_t(y0) * w + x0], b = px[size_t(y0) * w + x1], c = px[size_t(y1) * w + x0], d = px[size_t(y1) * w + x1];
		auto ch = [&](int sh) {
			const float t = ((a >> sh) & 0xff) * (1 - fx) + ((b >> sh) & 0xff) * fx, u = ((c >> sh) & 0xff) * (1 - fx) + ((d >> sh) & 0xff) * fx;
			return uint32_t(t * (1 - fy) + u * fy + 0.5f);
		};
		return 0xff000000u | ch(16) << 16 | ch(8) << 8 | ch(0);
	}
};

// The cover's region (sx, sy, sw, sh) scaled to cover the rectangle (x, y, w, h) of `dst` (cropped to fill).
void Fill(Img& dst, int x, int y, int w, int h, const Src& s, float sx, float sy, float sw, float sh)
{
	const float scale = std::max(w / sw, h / sh);
	const float cx = sx + sw / 2, cy = sy + sh / 2;
	for (int yy = std::max(0, y); yy < std::min(dst.h, y + h); yy++)
		for (int xx = std::max(0, x); xx < std::min(dst.w, x + w); xx++)
			dst.at(xx, yy) = s.at(cx + (xx - x - w / 2.0f) / scale, cy + (yy - y - h / 2.0f) / scale);
}

// The whole cover inside (x, y, w, h), its proportions kept (nothing cut off); the rest of the rectangle is the cover
// itself, enlarged to fill it, blurred and darker (no plain bars).
void Fit(Img& dst, int x, int y, int w, int h, const Src& s)
{
	Fill(dst, x, y, w, h, s, 0, 0, float(s.w), float(s.h));
	// the background: two box blurs (rows, then columns), darkened
	const int r = 7;
	std::vector<uint32_t> tmp(size_t(w) * h);
	auto blur = [&](bool rows) {
		for (int j = 0; j < (rows ? h : w); j++)
			for (int i = 0; i < (rows ? w : h); i++)
			{
				int sum[3] = {0, 0, 0}, n = 0;
				for (int k = -r; k <= r; k++)
				{
					const int ii = std::clamp(i + k, 0, (rows ? w : h) - 1);
					const uint32_t c = rows ? dst.at(x + ii, y + j) : tmp[size_t(ii) * w + j];
					sum[0] += R(c), sum[1] += G(c), sum[2] += B(c), n++;
				}
				const uint32_t c = Px(sum[0] / n, sum[1] / n, sum[2] / n);
				if (rows)
					tmp[size_t(j) * w + i] = c;
				else
					dst.at(x + j, y + i) = Px(R(c) * 55 / 100, G(c) * 55 / 100, B(c) * 55 / 100);
			}
	};
	blur(true);
	blur(false);
	// the cover, whole
	const float sc = std::min(w / float(s.w), h / float(s.h));
	const int fw = int(s.w * sc), fh = int(s.h * sc), fx = x + (w - fw) / 2, fy = y + (h - fh) / 2;
	for (int yy = std::max(0, fy); yy < std::min(dst.h, fy + fh); yy++)
		for (int xx = std::max(0, fx); xx < std::min(dst.w, fx + fw); xx++)
			dst.at(xx, yy) = s.at((xx - fx + 0.5f) / sc - 0.5f, (yy - fy + 0.5f) / sc - 0.5f);
}

// The whole cover stretched over (x, y, w, h).
void Stretch(Img& dst, int x, int y, int w, int h, const Src& s)
{
	for (int yy = std::max(0, y); yy < std::min(dst.h, y + h); yy++)
		for (int xx = std::max(0, x); xx < std::min(dst.w, x + w); xx++)
			dst.at(xx, yy) = s.at((xx - x + 0.5f) * s.w / w - 0.5f, (yy - y + 0.5f) * s.h / h - 0.5f);
}

void Rect(Img& d, int x, int y, int w, int h, uint32_t c)
{
	for (int yy = std::max(0, y); yy < std::min(d.h, y + h); yy++)
		for (int xx = std::max(0, x); xx < std::min(d.w, x + w); xx++)
			d.at(xx, yy) = c;
}

// The MSX lettering (white on black badge, or just the letters) in (x, y, w, h).
void Badge(Img& d, int x, int y, int w, int h, bool box, uint32_t fg, uint32_t bg)
{
	if (box)
		Rect(d, x, y, w, h, bg);
	const int pad = box ? std::max(2, h / 7) : 0;
	const int lw = w - 2 * pad, lh = h - 2 * pad;
	for (int yy = 0; yy < lh; yy++)
		for (int xx = 0; xx < lw; xx++)
		{
			const int a = kMsxLogo[size_t(yy * kMsxLogoH / lh) * kMsxLogoW + size_t(xx * kMsxLogoW / lw)];
			if (!a)
				continue;
			uint32_t& p = d.at(std::min(d.w - 1, x + pad + xx), std::min(d.h - 1, y + pad + yy));
			p = Px((R(p) * (255 - a) + R(fg) * a) / 255, (G(p) * (255 - a) + G(fg) * a) / 255, (B(p) * (255 - a) + B(fg) * a) / 255);
		}
}

// Text centred in a box, shrunk to fit its width.
void Title(Img& d, int x, int y, int w, int h, const std::string& s, uint32_t c, bool outline)
{
	int scale = std::max(1, h / 12);
	while (scale > 1 && TextWidth(s.c_str(), scale) > w)
		scale--;
	const std::string fit = FitText(s, scale, w);
	Canvas cv = d.canvas();
	DrawTextOn(cv, x + (w - TextWidth(fit.c_str(), scale)) / 2, y + (h - kGlyphH * scale) / 2, fit.c_str(), scale, c, outline);
}

// The game's publisher and year (blueMSX's msxromdb: msx_facts.inc), by its title; null: not known.
const char* const* Facts(const GameInfo& g)
{
	std::string t = gamedb::Title(g.nointro.empty() ? g.file_base : g.nointro), key;
	for (char& c : t)
		c = char(tolower(uint8_t(c)));
	if (t.compare(0, 4, "the ") == 0)
		t = t.substr(4);
	for (char c : t)
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			key += c;
	int lo = 0, hi = int(sizeof(kMsxFacts) / sizeof(kMsxFacts[0])) - 1;
	while (lo <= hi)
	{
		const int mid = (lo + hi) / 2, c = key.compare(kMsxFacts[mid][0]);
		if (c == 0)
			return kMsxFacts[mid];
		if (c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return nullptr;
}

// A screen in a white frame (stretched into it).
void Framed(Img& d, int x, int y, int w, int h, const Shot& s)
{
	Rect(d, x - 3, y - 3, w + 6, h + 6, Px(240, 240, 244));
	Rect(d, x - 1, y - 1, w + 2, h + 2, Px(20, 20, 24));
	const Src src{s.px.data(), s.w, s.h};
	for (int yy = std::max(0, y); yy < std::min(d.h, y + h); yy++)
		for (int xx = std::max(0, x); xx < std::min(d.w, x + w); xx++)
			d.at(xx, yy) = src.at((xx - x + 0.5f) * s.w / w - 0.5f, (yy - y + 0.5f) * s.h / h - 0.5f);
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}
} // namespace

bool IsKonami(const GameInfo& g)
{
	// Konami's MSX games (the cartridges with the strip and the two holes)
	static const char* const kTitles[] = {"gradius", "nemesis", "salamander", "parodius", "king's valley", "kings valley",
		"knightmare", "majou densetsu", "metal gear", "twin bee", "twinbee", "penguin adventure", "yume tairiku",
		"antarctic adventure", "nankyoku", "hyper sports", "hyper olympic", "track & field", "yie ar kung", "road fighter",
		"vampire killer", "akumajou", "castlevania", "usas", "maze of galious", "galious", "space manbow", "f1 spirit",
		"goonies", "q*bert", "qbert", "pippols", "hyper rally", "comic bakery", "sky jaguar", "time pilot", "frogger",
		"circus charlie", "mopiranger", "monkey academy", "juno first", "konami's", "magical tree", "athletic land",
		"contra", "gryzor", "snatcher", "solid snake", "game master", "quarth", "king kong", "shalom", "ganbare goemon",
		"goemon", "sparkie", "super cobra", "video hustler", "tennis", "billiards", "golf", "soccer", "boxing", "ping-pong",
		"ping pong", "pennant race", "dragon slayer", "nemesis 2", "nemesis 3", "gofer no yabou", "the treasure of usas",
		"hinotori", "firebird", "mon mon monster", "hai no majutsushi", "kingkong", "golvellius", "tsuri"};
	const std::string t = Lower(g.title + " " + g.nointro + " " + g.file_base);
	if (t.find("konami") != std::string::npos)
		return true;
	for (const char* k : kTitles)
		if (t.find(k) != std::string::npos)
			return true;
	return false;
}

cart3d::Kind MediaOf(const GameInfo& g)
{
	if (g.system == System::Disk)
		return cart3d::Kind::Floppy; // (a disk image in a .zip too)
	const std::string e = Lower(g.ext);
	return (e == ".dsk" || e == ".di1" || e == ".di2" || e == ".fdi") ? cart3d::Kind::Floppy : cart3d::Kind::Cartridge;
}

std::shared_ptr<const cart3d::Art> MakeArt(const uint32_t* rgba, int w, int h, const GameInfo& g, const Shots* shots,
	const uint32_t* box_px, int box_w, int box_h)
{
	auto art = std::make_shared<cart3d::Art>();
	const Src cov{rgba, w, h};
	// the box's own picture (portrait), else the cover
	const uint32_t* fp = box_px ? box_px : rgba;
	const int fw = box_px ? box_w : w, fh = box_px ? box_h : h;
	const Src fcov{fp, fw, fh};
	art->front = cart3d::Texture::FromPixels(fp, fw, fh);
	const uint32_t avg = art->front->average;
	const std::string title = g.title.empty() ? g.file_base : g.title;

	// the spine: the cover's colour, darker; the title running up; the MSX badge at the foot
	{
		const int sw = 112, sh = 768;
		Img sp(sw, sh);
		for (int y = 0; y < sh; y++)
		{
			const float k = 0.32f + 0.12f * (1 - float(y) / sh);
			for (int x = 0; x < sw; x++)
				sp.at(x, y) = Px(int(R(avg) * k), int(G(avg) * k), int(B(avg) * k));
		}
		Rect(sp, 0, 0, sw, 6, Px(230, 230, 236)), Rect(sp, 0, sh - 6, sw, 6, Px(230, 230, 236));
		// the title, drawn across then turned to run up the spine
		Img t(sh - 200, sw);
		for (uint32_t& p : t.px)
			p = 0;
		Title(t, 10, 18, t.w - 20, sw - 36, title, Px(245, 245, 250), true);
		for (int y = 0; y < t.h; y++)
			for (int x = 0; x < t.w; x++)
				if (t.at(x, y) != 0)
					sp.at(y, sh - 170 - x) = t.at(x, y);
		Badge(sp, 14, sh - 150, sw - 28, 40, true, Px(250, 250, 250), Px(10, 10, 12));
		art->spine = cart3d::Texture::FromPixels(sp.px.data(), sw, sh);
	}

	// the back, as a real box's: the cover dark and soft behind; the title; the publisher, the year, the media; the
	// game's screens - a big in-game picture, a row of smaller ones (the title screen, other pictures); the MSX
	// badge and a barcode at the foot
	{
		const bool wide = fw > fh;
		const int bw = wide ? 512 : 384, bh = std::clamp(int(float(bw) * fh / fw), 240, 768);
		Img bk(bw, bh);
		for (int y = 0; y < bh; y++)
			for (int x = 0; x < bw; x++)
			{
				int r = 0, gg = 0, b = 0;
				for (int k = 0; k < 9; k++)
				{
					const uint32_t p = fcov.at((x + (k % 3 - 1) * 4) * float(fw) / bw, (y + (k / 3 - 1) * 4) * float(fh) / bh);
					r += R(p), gg += G(p), b += B(p);
				}
				bk.at(x, y) = Px(r / 9 / 4 + 6, gg / 9 / 4 + 6, b / 9 / 4 + 10);
			}
		Title(bk, 14, 10, bw - 28, 40, title, Px(245, 245, 250), true);
		// the publisher and year, the media
		std::string line;
		if (const char* const* f = Facts(g))
			line = std::string(f[1]) + (f[1][0] && f[2][0] ? "  -  " : "") + f[2];
		const std::string media = MediaOf(g) == cart3d::Kind::Floppy ? "3.5\" disk"
			: g.system == System::Msx2 ? "MSX2 cartridge" : "MSX cartridge";
		line += (line.empty() ? "" : "  -  ") + media;
		Title(bk, 14, 50, bw - 28, 22, line, Px(210, 210, 222), false);
		// the screens
		std::vector<const Shot*> small;
		const Shot* big = nullptr;
		if (shots)
		{
			for (const Shot& sh : shots->snaps)
				if (sh.w > 0)
				{
					if (big)
						small.push_back(&sh);
					else
						big = &sh;
				}
			if (shots->title.w > 0)
			{
				if (big)
					small.insert(small.begin(), &shots->title);
				else
					big = &shots->title;
			}
		}
		if (small.size() > 3)
			small.resize(3);
		const int top = 84, bottom = bh - 66;
		if (big)
		{
			if (wide)
			{
				// the big one left, the others in a column on the right
				int bwid = small.empty() ? bw - 40 : int((bw - 56) * 0.62f), bhei = bwid * 3 / 4;
				if (bhei > bottom - top)
					bhei = bottom - top, bwid = bhei * 4 / 3;
				Framed(bk, 20, top, bwid, bhei, *big);
				const int cx = 20 + bwid + 16, cw = bw - 20 - cx;
				if (!small.empty())
				{
					const int ch = std::min(cw * 3 / 4, (bottom - top - 12 * (int(small.size()) - 1)) / int(small.size()));
					for (size_t i = 0; i < small.size(); i++)
						Framed(bk, cx, top + int(i) * (ch + 12), std::min(cw, ch * 4 / 3), ch, *small[i]);
				}
			}
			else
			{
				// the big one on top, the others in a row under it
				const int rows_h = small.empty() ? 0 : 90;
				int bwid = bw - 40, bhei = bwid * 3 / 4;
				if (bhei > bottom - top - rows_h)
					bhei = bottom - top - rows_h, bwid = bhei * 4 / 3;
				Framed(bk, (bw - bwid) / 2, top, bwid, bhei, *big);
				if (!small.empty())
				{
					const int n = int(small.size()), gap = 12;
					const int sw = (bw - 40 - gap * (n - 1)) / n, sh = std::min(sw * 3 / 4, rows_h - 12);
					const int y = top + bhei + 16;
					for (int i = 0; i < n; i++)
						Framed(bk, 20 + i * (sw + gap), y, std::min(sw, sh * 4 / 3), sh, *small[size_t(i)]);
				}
			}
		}
		// the foot: the badge, a barcode
		Badge(bk, 18, bh - 50, 86, 32, true, Px(250, 250, 250), Px(10, 10, 12));
		{
			const int bx = bw - 128, by = bh - 54, bww = 110, bhh = 40;
			Rect(bk, bx, by, bww, bhh, Px(248, 248, 248));
			uint32_t hsh = 2166136261u;
			for (char c : title)
				hsh = (hsh ^ uint8_t(c)) * 16777619u;
			for (int x = bx + 6; x < bx + bww - 6;)
			{
				hsh = hsh * 1664525u + 1013904223u;
				const int bar = 1 + int((hsh >> 28) % 3), space = 1 + int((hsh >> 24) % 3);
				Rect(bk, x, by + 4, std::min(bar, bx + bww - 6 - x), bhh - 8, Px(12, 12, 14));
				x += bar + space;
			}
		}
		art->back = cart3d::Texture::FromPixels(bk.px.data(), bw, bh);
	}

	// the label
	const bool konami = IsKonami(g);
	art->konami = konami;
	{
		// a floppy's label (every game: the shelf of floppies): the art filling it, the badge
		const int lw = 480, lh = 330;
		Img lb(lw, lh, Px(245, 245, 245));
		Stretch(lb, 0, 0, lw, lh, cov);
		Badge(lb, lw - 92, lh - 40, 80, 30, true, Px(250, 250, 250), Px(10, 10, 12));
		art->disk_label = cart3d::Texture::FromPixels(lb.px.data(), lw, lh);
	}
	if (MediaOf(g) == cart3d::Kind::Floppy)
		art->label = art->disk_label;
	else if (konami)
	{
		// Konami's: the art in a coloured frame up to the top, an orange band, the badge in white
		const int lw = 640, lh = 504;
		Img lb(lw, lh, Px(240, 232, 205));
		const int foot = 74, frame = 10;
		Rect(lb, 0, 0, lw, lh - foot, Px(40, 150, 70));
		Stretch(lb, frame, frame, lw - 2 * frame, lh - foot - 2 * frame, cov);
		Rect(lb, 0, lh - foot, lw, foot, Px(245, 160, 30));
		Rect(lb, 0, lh - foot, lw, 6, Px(40, 60, 170));
		Canvas cv = lb.canvas();
		DrawTextOn(cv, 26, lh - foot + 18, "KONAMI", 4, Px(30, 30, 40), false);
		Badge(lb, lw - 160, lh - foot + 14, 130, 48, true, Px(250, 250, 250), Px(12, 12, 14));
		art->label = cart3d::Texture::FromPixels(lb.px.data(), lw, lh);
	}
	else
	{
		// generic: the art filling the label, the badge in a white corner
		const int lw = 640, lh = 380;
		Img lb(lw, lh);
		Stretch(lb, 0, 0, lw, lh, cov);
		Rect(lb, lw - 150, lh - 64, 150, 64, Px(245, 245, 245));
		Badge(lb, lw - 136, lh - 54, 122, 44, true, Px(250, 250, 250), Px(12, 12, 14));
		art->label = cart3d::Texture::FromPixels(lb.px.data(), lw, lh);
	}
	return art;
}
} // namespace fe
