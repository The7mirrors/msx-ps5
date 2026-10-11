// Preview of the 3D shelf objects (fe_cart3d) on the PC: cart3d_preview cover1.png [cover2.png ...] out.ppm
#include "../frontend/fe_cart3d.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../frontend/third_party/stb_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace cart3d;

static uint32_t Px(int r, int g, int b)
{
	return 0xff000000u | uint32_t(std::clamp(b, 0, 255)) << 16 | uint32_t(std::clamp(g, 0, 255)) << 8 | uint32_t(std::clamp(r, 0, 255));
}

struct Img
{
	int w = 0, h = 0;
	std::vector<uint32_t> px;
	uint32_t at(int x, int y) const { return px[size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]; }
};

static Img Load(const char* path)
{
	Img im;
	int c;
	unsigned char* d = stbi_load(path, &im.w, &im.h, &c, 4);
	im.px.assign(reinterpret_cast<uint32_t*>(d), reinterpret_cast<uint32_t*>(d) + size_t(im.w) * im.h);
	stbi_image_free(d);
	return im;
}

// The art set the app will make from a cover: label = the cover's top (where the logo usually is), spine and back from
// its colours.
static Art MakeArt(const Img& c)
{
	Art a;
	a.front = Texture::FromPixels(c.px.data(), c.w, c.h);
	const uint32_t avg = a.front->average;
	const int ar = avg & 0xff, ag = (avg >> 8) & 0xff, ab = (avg >> 16) & 0xff;
	// spine: a dark band in the cover's colour, a light rule
	{
		const int w = 64, h = 512;
		std::vector<uint32_t> s(size_t(w) * h);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
			{
				const float k = 0.35f + 0.1f * std::sin(x * 0.05f);
				uint32_t p = Px(int(ar * k), int(ag * k), int(ab * k));
				if (y > 40 && y < 44)
					p = Px(230, 230, 235);
				if (y > h - 90 && y < h - 30 && x > 14 && x < w - 14)
					p = Px(240, 240, 245); // the system badge
				s[size_t(y) * w + x] = p;
			}
		a.spine = Texture::FromPixels(s.data(), w, h);
	}
	// back: the cover, darkened and soft
	{
		const int w = c.w / 4, h = c.h / 4;
		std::vector<uint32_t> s(size_t(w) * h);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
			{
				int r = 0, g = 0, b = 0;
				for (int k = 0; k < 16; k++)
				{
					const uint32_t p = c.at(x * 4 + (k & 3) * 3, y * 4 + (k >> 2) * 3);
					r += p & 0xff, g += (p >> 8) & 0xff, b += (p >> 16) & 0xff;
				}
				s[size_t(y) * w + x] = Px(r / 16 / 3 + 10, g / 16 / 3 + 10, b / 16 / 3 + 14);
			}
		a.back = Texture::FromPixels(s.data(), w, h);
	}
	// label (Konami style, 1.27 : 1): a coloured frame, a cream title band (the cover's logo), the art, the MSX badge
	{
		const int lw = 640, lh = int(640 / 1.27f);
		std::vector<uint32_t> s(size_t(lw) * lh, Px(240, 232, 205));
		const int band = int(lh * 0.16f), frame = 10;
		// the title band: the top of the cover (its logo), fitted into the band's height
		const float k = float(band - 8) / (c.h * 0.22f);
		const int tw = int(c.w * k), tx0 = (lw - tw) / 2;
		for (int y = 4; y < band - 4; y++)
			for (int x = std::max(0, tx0); x < std::min(lw, tx0 + tw); x++)
				s[size_t(y) * lw + x] = c.at(int((x - tx0) / k), int((y - 4) / k));
		// the art: the cover's middle, filling the framed area
		const int ay0 = band + frame, ay1 = lh - frame, ax0 = frame, ax1 = lw - frame;
		const float sc = std::max(float(ax1 - ax0) / c.w, float(ay1 - ay0) / (c.h * 0.7f));
		for (int y = band; y < lh; y++)
			for (int x = 0; x < lw; x++)
			{
				uint32_t p = Px(40, 150, 70); // the frame
				if (y >= ay0 && y < ay1 && x >= ax0 && x < ax1)
					p = c.at(int(c.w * 0.5f + (x - (ax0 + ax1) * 0.5f) / sc), int(c.h * 0.58f + (y - (ay0 + ay1) * 0.5f) / sc));
				s[size_t(y) * lw + x] = p;
			}
		// the white corner with the system badge, bottom right
		for (int y = lh - 70; y < lh; y++)
			for (int x = lw - 150; x < lw; x++)
				s[size_t(y) * lw + x] = (y > lh - 52 && y < lh - 18 && x > lw - 128 && x < lw - 26) ? Px(20, 20, 24) : Px(245, 245, 245);
		a.label = Texture::FromPixels(s.data(), lw, lh);
	}
	return a;
}

static void Backdrop(std::vector<uint32_t>& f, const View& v)
{
	const float floor_y = v.horizon + v.focal * v.eye_y / 1000.0f;
	for (int y = 0; y < v.h; y++)
		for (int x = 0; x < v.w; x++)
		{
			const float vx = (x - v.w * 0.5f) / (v.w * 0.6f), vy = (y - v.h * 0.45f) / (v.h * 0.8f);
			const float vig = std::max(0.3f, 1 - 0.6f * (vx * vx + vy * vy));
			float r = 22, g = 18, b = 44;
			if (y > floor_y)
			{
				const float k = 0.7f - 0.3f * std::min(1.0f, (y - floor_y) / (v.h - floor_y));
				r *= k, g *= k, b *= k;
			}
			f[size_t(y) * v.w + x] = Px(int(r * vig), int(g * vig), int(b * vig));
		}
}

static void Save(const std::vector<uint32_t>& f, const View& v, const std::string& path)
{
	FILE* o = std::fopen(path.c_str(), "wb");
	std::fprintf(o, "P6\n%d %d\n255\n", v.w, v.h);
	for (uint32_t p : f)
	{
		const unsigned char rgb[3] = {uint8_t(p), uint8_t(p >> 8), uint8_t(p >> 16)};
		std::fwrite(rgb, 1, 3, o);
	}
	std::fclose(o);
}

int main(int argc, char** argv)
{
	std::vector<Art> arts;
	for (int i = 1; i + 1 < argc; i++)
		arts.push_back(MakeArt(Load(argv[i])));
	const std::string out = argv[argc - 1];
	View v;
	std::vector<uint32_t> f(size_t(v.w) * v.h);

	// 1. the shelf: boxes, the middle one turning a little
	Backdrop(f, v);
	auto t0 = std::chrono::steady_clock::now();
	struct Item
	{
		float d;
	};
	std::vector<std::pair<float, int>> order;
	for (int i = -3; i <= 3; i++)
		order.push_back({std::fabs(float(i)), i});
	std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
	for (auto& o : order)
	{
		const int i = o.second;
		Pose p;
		const float s = i < 0 ? -1.0f : 1.0f, ad = std::fabs(float(i));
		p.height = 520;
		if (i == 0)
		{
			p.x = 0, p.z = 1000, p.yaw = -0.32f;
		}
		else
		{
			p.x = s * (560 + (ad - 1) * 190);
			p.z = 1000 + 300;
			p.yaw = s * 1.0f; // facing the middle
			p.bright = 1 - 0.12f * ad;
		}
		Draw(f.data(), v, Kind::Box, p, arts[size_t((i + 3) % int(arts.size()))], 0, v.w);
	}
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	std::printf("shelf: %.1f ms (one thread)\n", ms);
	Save(f, v, out + "-shelf.ppm");

	// 2. the cartridge coming out, and a floppy
	Backdrop(f, v);
	{
		Pose box;
		box.x = -430, box.z = 1150, box.yaw = 0.42f, box.height = 520, box.bright = 0.9f;
		Draw(f.data(), v, Kind::Box, box, arts[0], 0, v.w);
		Pose cart;
		cart.x = 140, cart.z = 900, cart.yaw = -0.52f, cart.pitch = 0.04f, cart.height = 300, cart.lift = 0;
		Draw(f.data(), v, Kind::Cartridge, cart, arts[0], 0, v.w);
		Pose fl;
		fl.x = 620, fl.z = 1100, fl.yaw = -0.55f, fl.height = 330;
		Draw(f.data(), v, Kind::Floppy, fl, arts[arts.size() > 1 ? 1 : 0], 0, v.w);
	}
	Save(f, v, out + "-cart.ppm");

	// 3. close-ups of the cartridge: from the left, and from the right
	Backdrop(f, v);
	{
		Pose c1;
		c1.x = -430, c1.z = 760, c1.yaw = 0.32f, c1.pitch = 0.0f, c1.height = 330;
		Draw(f.data(), v, Kind::Cartridge, c1, arts[0], 0, v.w);
		Pose c2;
		c2.x = 470, c2.z = 820, c2.yaw = -0.62f, c2.height = 330;
		Draw(f.data(), v, Kind::Cartridge, c2, arts[arts.size() > 2 ? 2 : 0], 0, v.w);
	}
	Save(f, v, out + "-close.ppm");

	// 4. the back, and the bottom (tipped towards the camera)
	Backdrop(f, v);
	{
		Pose b;
		b.x = -60, b.z = 700, b.yaw = 3.14159f - 0.22f, b.pitch = 0.12f, b.height = 420;
		Draw(f.data(), v, Kind::Cartridge, b, arts[0], 0, v.w);

	}
	Save(f, v, out + "-back.ppm");

	// 5. Konami's cartridge and a generic one side by side
	Backdrop(f, v);
	{
		Art k = arts[0];
		k.konami = true;
		Pose a1;
		a1.x = -430, a1.z = 880, a1.yaw = 0.35f, a1.height = 320;
		Draw(f.data(), v, Kind::Cartridge, a1, k, 0, v.w);
		Pose a2;
		a2.x = 430, a2.z = 880, a2.yaw = -0.35f, a2.height = 320;
		Draw(f.data(), v, Kind::Cartridge, a2, arts[arts.size() > 3 ? 3 : 0], 0, v.w);
	}
	Save(f, v, out + "-two.ppm");
	return 0;
}
