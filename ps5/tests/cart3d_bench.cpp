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


#include <thread>
#include <atomic>
// The shelf as fe_shelf draws it: 7.5 boxes each side + the middle one, in 6 column bands.
static void Frame(std::vector<uint32_t>& f, const View& v, const std::vector<Art>& arts, float pos, int x0, int x1)
{
	const float kSideX = 600, kSideStep = 165, kSideZ = 330, kSideAngle = 1.10f, kVisible = 7.5f, kCoverH = 540;
	std::vector<std::pair<float, int>> order;
	for (int i = int(std::floor(pos - kVisible)); i <= int(std::ceil(pos + kVisible)); i++)
		order.push_back({std::fabs(i - pos), i});
	std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
	for (auto& o : order)
	{
		const float d = o.second - pos, ad = std::fabs(d), sgn = d < 0 ? -1.0f : 1.0f;
		const float k = std::min(ad, 1.0f), e = k * k * (3 - 2 * k);
		Pose p;
		p.x = ad <= 1 ? sgn * kSideX * e : sgn * (kSideX + (ad - 1) * kSideStep);
		p.z = 1000 + kSideZ * e;
		p.yaw = sgn * kSideAngle * e;
		p.height = kCoverH;
		p.bright = std::clamp(1.0f - 0.40f * e - 0.07f * std::max(0.0f, ad - 1), 0.25f, 1.0f);
		p.alpha = std::clamp(kVisible - ad, 0.0f, 1.0f);
		if (p.alpha <= 0) continue;
		Draw(f.data(), v, Kind::Box, p, arts[size_t(((o.second % 5) + 5) % 5)], x0, x1);
	}
}

// The new way: side games (|d| >= 1) as sprites drawn once for their side, the middle one live.
static const float kSideX = 600, kSideStep = 165, kSideZ = 330, kSideAngle = 1.10f, kVisible = 7.5f, kCoverH = 540;
static Pose SidePose(int side)
{
	Pose q;
	q.x = side * kSideX, q.z = 1000 + kSideZ, q.yaw = side * kSideAngle, q.height = kCoverH, q.bright = 0.6f;
	return q;
}
static void Frame2(std::vector<uint32_t>& f, const View& v, const std::vector<Art>& arts, std::vector<Sprite> (&spr)[2], float pos, int x0, int x1)
{
	std::vector<std::pair<float, int>> order;
	for (int i = int(std::floor(pos - kVisible)); i <= int(std::ceil(pos + kVisible)); i++)
		order.push_back({std::fabs(i - pos), i});
	std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
	for (auto& o : order)
	{
		const float d = o.second - pos, ad = std::fabs(d), sgn = d < 0 ? -1.0f : 1.0f;
		const int ai = ((o.second % 5) + 5) % 5;
		const float alpha = std::clamp(kVisible - ad, 0.0f, 1.0f);
		if (alpha <= 0) continue;
		const float b = std::clamp(1.0f - 0.40f - 0.07f * std::max(0.0f, ad - 1), 0.25f, 1.0f);
		if (ad >= 0.999f)
		{
			const int dx = int(std::lround(1000 * sgn * (ad - 1) * kSideStep / (1000 + kSideZ)));
			Blit(f.data(), v, spr[d < 0 ? 0 : 1][size_t(ai)], dx, std::sqrt(b / 0.6f), alpha, x0, x1);
			continue;
		}
		const float k = std::min(ad, 1.0f), e = k * k * (3 - 2 * k);
		Pose p;
		p.x = sgn * kSideX * e, p.z = 1000 + kSideZ * e, p.yaw = sgn * kSideAngle * e, p.height = kCoverH;
		p.bright = std::clamp(1.0f - 0.40f * e, 0.25f, 1.0f);
		Draw(f.data(), v, Kind::Box, p, arts[size_t(ai)], x0, x1);
	}
}
template <class F> static double Par(F fn)
{
	auto t0 = std::chrono::steady_clock::now();
	std::vector<std::thread> th;
	std::atomic<int> next{0};
	for (int i = 0; i < 6; i++)
		th.emplace_back([&] { for (;;) { int s = next++; if (s * 64 >= 1920) return; fn(s * 64, std::min(1920, s * 64 + 64)); } });
	for (auto& t : th) t.join();
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
int main(int argc, char** argv)
{
	std::vector<Art> arts;
	for (int i = 1; i + 1 < argc; i++)
		arts.push_back(MakeArt(Load(argv[i])));
	const std::string out = argv[argc - 1];
	View v;
	std::vector<uint32_t> f(size_t(v.w) * v.h);
	for (float pos : {0.0f, 0.37f})
	{
		Backdrop(f, v);
		auto t0 = std::chrono::steady_clock::now();
		Frame(f, v, arts, pos, 0, v.w);
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::printf("pos %.2f: one thread %.1f ms\n", pos, ms);
		Backdrop(f, v);
		t0 = std::chrono::steady_clock::now();
		std::vector<std::thread> th;
		const int n = 6, step = ((v.w / n) + 31) & ~31;
		for (int i = 0; i < n; i++)
			th.emplace_back([&, i] { Frame(f, v, arts, pos, i * step, std::min(v.w, (i + 1) * step)); });
		for (auto& t : th) t.join();
		ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::printf("pos %.2f: 6 bands %.1f ms\n", pos, ms);
		Save(f, v, out + (pos == 0 ? "-b0.ppm" : "-b1.ppm"));
	}
	// the new way
	std::vector<Sprite> spr[2];
	double tmake = 0;
	for (int side = 0; side < 2; side++)
		for (size_t a = 0; a < arts.size(); a++)
		{
			Sprite sp;
			const Pose q = SidePose(side ? 1 : -1);
			Prepare(sp, v, Kind::Box, q);
			tmake += Par([&](int x0, int x1) { Render(sp, v, Kind::Box, q, arts[a], sp.x0 + x0 * sp.w / 1920, sp.x0 + x1 * sp.w / 1920); });
			spr[side].push_back(std::move(sp));
		}
	std::printf("sprites: %.2f ms each (%dx%d)\n", tmake / 10, spr[0][0].w, spr[0][0].h);
	for (float pos : {0.0f, 0.37f})
	{
		Backdrop(f, v);
		const double ms = Par([&](int x0, int x1) { Frame2(f, v, arts, spr, pos, x0, x1); });
		std::printf("new pos %.2f: %.2f ms (6 threads, strips)\n", pos, ms);
		Save(f, v, out + (pos == 0 ? "-n0.ppm" : "-n1.ppm"));
	}
}
