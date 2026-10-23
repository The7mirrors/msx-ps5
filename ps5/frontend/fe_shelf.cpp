// MSX PS5 frontend: the 3D game shelf (fe_shelf.h).
// SPDX-License-Identifier: MIT

#include "fe_shelf.h"

#include "fe_art.h"
#include "fe_cart3d.h"
#include "fe_covers.h"
#include "fe_games.h"
#include "fe_floppy.h"
#include "fe_menu.h"
#include "fe_prefetch.h"
#include "fe_settings.h"
#include "fe_text.h"

#include "OrbisPaths.h"
#include "ProsperoInput.h"
#include "ProsperoMic.h"
#include "fe_sound.h"
#include "ProsperoSce.h"
#include "ProsperoCrash.h"
#include "ProsperoThread.h"
#include "ProsperoVideo.h"
#if defined(MSX_ANDROID)
#include "msx_android.h"
#endif

#if defined(__x86_64__)
#include <immintrin.h>
#endif

#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#ifndef GENPLUS_PS5_VERSION
#define GENPLUS_PS5_VERSION "dev"
#endif

namespace fe
{
namespace
{
using ps5video::Rgb;
constexpr int W = ps5video::kWidth;
constexpr int H = ps5video::kHeight;

// ---- camera and layout (world units: the middle cover is kCoverH tall at distance kCamDist) ----------
constexpr float kFocal = 1000.0f;
constexpr float kCamDist = 1000.0f;
constexpr float kCoverH = 540.0f;
constexpr float kCamY = kCoverH * 0.42f; // eye height above the floor
constexpr float kHorizon = 455.0f; // screen row of the eye
constexpr float kSideX = 600.0f; // first side cover
constexpr float kSideStep = 165.0f; // the next ones
constexpr float kSideZ = 330.0f; // how far back the side covers stand
constexpr float kSideAngle = 1.10f; // radians (63 degrees)
constexpr float kVisible = 7.5f; // covers each side
constexpr float kReflect = 0.42f; // reflection length, of the cover's height
constexpr int kKeepRadius = 14; // textures kept around the selection

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

inline uint32_t Lerp32(uint32_t a, uint32_t b, uint32_t w) // w 0..256
{
	const uint32_t iw = 256 - w;
	const uint32_t rb = (((a & 0xff00ffu) * iw + (b & 0xff00ffu) * w) >> 8) & 0xff00ffu;
	const uint32_t g = (((a & 0x00ff00u) * iw + (b & 0x00ff00u) * w) >> 8) & 0x00ff00u;
	return 0xff000000u | rb | g;
}

inline uint32_t Scale32(uint32_t a, uint32_t s) // s 0..256
{
	const uint32_t rb = (((a & 0xff00ffu) * s) >> 8) & 0xff00ffu;
	const uint32_t g = (((a & 0x00ff00u) * s) >> 8) & 0x00ff00u;
	return 0xff000000u | rb | g;
}

inline uint32_t Add32(uint32_t a, uint32_t b) // per channel, saturating
{
#if defined(__x86_64__)
	return uint32_t(_mm_cvtsi128_si32(_mm_adds_epu8(_mm_cvtsi32_si128(int(a)), _mm_cvtsi32_si128(int(b))))) | 0xff000000u;
#else
	uint32_t r = 0;
	for (int sh = 0; sh < 24; sh += 8)
	{
		const uint32_t c = ((a >> sh) & 0xff) + ((b >> sh) & 0xff);
		r |= (c > 255 ? 255u : c) << sh;
	}
	return r | 0xff000000u;
#endif
}

// ---- a small pool: the screen in 64-pixel columns, handed out to the threads as they are free ------------
class Pool
{
public:
	Pool()
	{
		unsigned n = std::thread::hardware_concurrency();
		n = std::clamp(n == 0 ? 4u : n, 2u, 6u);
		for (unsigned i = 1; i < n; i++)
			m_threads.emplace_back([this, i] { Worker(int(i)); }, 2 * 1024 * 1024);
		m_count = int(n);
	}
	~Pool()
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_quit = true;
			m_gen++;
		}
		m_cv.notify_all();
		for (auto& t : m_threads)
			t.join();
	}
	// job(x0, x1) on every band, the calling thread included; returns when all are done.
	void Run(const std::function<void(int, int)>& job)
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_job = &job;
			m_pending = m_count - 1;
			m_next = 0;
			m_gen++;
		}
		m_cv.notify_all();
		Strips(job);
		std::unique_lock<std::mutex> lock(m_lock);
		m_done.wait(lock, [this] { return m_pending == 0; });
		m_job = nullptr;
	}

private:
	static constexpr int kStrip = 64;
	void Strips(const std::function<void(int, int)>& job)
	{
		for (;;)
		{
			const int x0 = m_next.fetch_add(1) * kStrip;
			if (x0 >= W)
				return;
			job(x0, std::min(W, x0 + kStrip));
		}
	}
	void Worker(int)
	{
		uint64_t seen = 0;
		for (;;)
		{
			const std::function<void(int, int)>* job;
			{
				std::unique_lock<std::mutex> lock(m_lock);
				m_cv.wait(lock, [&] { return m_gen != seen; });
				seen = m_gen;
				if (m_quit)
					return;
				job = m_job;
			}
			if (job)
				Strips(*job);
			{
				std::lock_guard<std::mutex> lock(m_lock);
				if (--m_pending == 0)
					m_done.notify_one();
			}
		}
	}
	std::vector<ps5::BigThread> m_threads;
	int m_count = 1;
	std::mutex m_lock;
	std::condition_variable m_cv, m_done;
	uint64_t m_gen = 0;
	int m_pending = 0;
	bool m_quit = false;
	const std::function<void(int, int)>* m_job = nullptr;
	std::atomic<int> m_next{0};
};

// ---- the backdrop: the selected cover, blurred, darkened, with a floor and a vignette -----------------
struct Backdrop
{
	std::vector<uint32_t> px; // W*H
	const CoverTex* from = nullptr;
};

void BuildBackdrop(Pool& pool, Backdrop& bd, const CoverTex* tex, bool plain = false)
{
	bd.px.resize(size_t(W) * H);
	bd.from = tex;
	// 1. a 64x36 thumbnail: the cover's smallest level, scaled to fill (cropped), or a plain colour
	constexpr int TW = 64, TH = 36;
	float small[TH][TW][3];
	for (int y = 0; y < TH; y++)
		for (int x = 0; x < TW; x++)
		{
			uint32_t p = Rgb(28, 22, 60);
			if (tex)
			{
				const int l = CoverTex::kLevels - 1;
				const int tw = tex->w[l], th = tex->h[l];
				const float s = std::max(float(TW) / tw, float(TH) / th);
				const int sx = std::clamp(int((x - TW / 2.0f) / s + tw / 2.0f), 0, tw - 1);
				const int sy = std::clamp(int((y - TH / 2.0f) / s + th / 2.0f), 0, th - 1);
				p = tex->px[l][size_t(sx) * th + sy];
			}
			small[y][x][0] = float(p & 0xff);
			small[y][x][1] = float((p >> 8) & 0xff);
			small[y][x][2] = float((p >> 16) & 0xff);
		}
	// 2. blur: three box passes each way
	for (int pass = 0; pass < 3; pass++)
	{
		float tmp[TH][TW][3];
		for (int y = 0; y < TH; y++)
			for (int x = 0; x < TW; x++)
				for (int c = 0; c < 3; c++)
				{
					float s = 0;
					int n = 0;
					for (int k = -3; k <= 3; k++)
					{
						const int xx = std::clamp(x + k, 0, TW - 1);
						s += small[y][xx][c];
						n++;
					}
					tmp[y][x][c] = s / n;
				}
		for (int y = 0; y < TH; y++)
			for (int x = 0; x < TW; x++)
				for (int c = 0; c < 3; c++)
				{
					float s = 0;
					int n = 0;
					for (int k = -3; k <= 3; k++)
					{
						const int yy = std::clamp(y + k, 0, TH - 1);
						s += tmp[yy][x][c];
						n++;
					}
					small[y][x][c] = s / n;
				}
	}
	// 3. darken towards a deep blue-violet, so text stays readable whatever the art
	const float base[3] = {10, 9, 22};
	for (int y = 0; y < TH; y++)
		for (int x = 0; x < TW; x++)
			for (int c = 0; c < 3; c++)
				small[y][x][c] = small[y][x][c] * 0.36f + base[c] * 0.64f + 6;
	// 4. scale up (bilinear) with a vignette and a darker floor below the covers
	const float floor_y = kHorizon + kFocal * kCamY / kCamDist;
	pool.Run([&](int x0, int x1) {
		for (int y = 0; y < H; y++)
		{
			const float fy = (y + 0.5f) * TH / H - 0.5f;
			const int y0 = std::clamp(int(std::floor(fy)), 0, TH - 1), y1 = std::min(y0 + 1, TH - 1);
			const float wy = std::clamp(fy - y0, 0.0f, 1.0f);
			const float vy = (y - H * 0.45f) / (H * 0.75f);
			float floor_k = 1.0f;
			if (y > floor_y && !plain)
				floor_k = 0.78f - 0.25f * std::min(1.0f, (y - floor_y) / (H - floor_y));
			uint32_t* row = &bd.px[size_t(y) * W];
			for (int x = x0; x < x1; x++)
			{
				const float fx = (x + 0.5f) * TW / W - 0.5f;
				const int xa = std::clamp(int(std::floor(fx)), 0, TW - 1), xb = std::min(xa + 1, TW - 1);
				const float wx = std::clamp(fx - xa, 0.0f, 1.0f);
				const float vx = (x - W * 0.5f) / (W * 0.62f);
				const float vig = std::max(0.25f, 1.0f - 0.55f * (vx * vx + vy * vy));
				int ch[3];
				for (int c = 0; c < 3; c++)
				{
					const float top = small[y0][xa][c] * (1 - wx) + small[y0][xb][c] * wx;
					const float bot = small[y1][xa][c] * (1 - wx) + small[y1][xb][c] * wx;
					ch[c] = int((top * (1 - wy) + bot * wy) * vig * floor_k);
				}
				row[x] = Rgb(uint8_t(std::min(255, ch[0])), uint8_t(std::min(255, ch[1])), uint8_t(std::min(255, ch[2])));
			}
		}
	});
	// a faint horizon line on the floor
	if (plain)
		return;
	const int fy = int(floor_y);
	for (int x = 0; x < W; x++)
	{
		const float k = 1.0f - std::abs(x - W * 0.5f) / (W * 0.5f);
		uint32_t& p = bd.px[size_t(fy) * W + x];
		p = Add32(p, Rgb(uint8_t(30 * k), uint8_t(26 * k), uint8_t(52 * k)));
	}
}

// ---- the themes' backdrops: a room in the shelf's own perspective (the eye kCamY over the floor, kFocal) ------
#include "msx_real_logo.inc"

struct ThemeLook
{
	float wall_top[3], wall_base[3]; // the back wall (or sky): top row, at the floor
	float floor_far[3], floor_near[3];
	float line[3];                   // the grid lines (added)
	float line_k;                    // their strength
	bool wall_grid, floor_grid;
};

inline float LineAt(float v, float spacing, float footprint)
{
	// how near v is to a grid line (0..1), the line about one pixel wide whatever the distance
	const float f = v / spacing;
	const float d = std::abs(f - std::round(f)) * spacing;
	// (solid for about a pixel and a half, then a soft edge: the top picture's lines are 2-3 px wide)
	return std::clamp(1.0f - (d - footprint * 0.75f) / std::max(footprint, 0.001f), 0.0f, 1.0f);
}

void BuildThemeBackdrop(Pool& pool, Backdrop& bd, int theme)
{
	bd.px.resize(size_t(W) * H);
	bd.from = nullptr;
	static const ThemeLook kLooks[5] = {
		{},
		{{50, 120, 230}, {47, 106, 221}, {46, 102, 218}, {40, 80, 200}, {238, 244, 255}, 1.0f, true, true}, // Sakhr blue (the top picture's)
		{{4, 4, 6}, {14, 14, 18}, {16, 16, 20}, {3, 3, 4}, {120, 120, 130}, 0.18f, false, true},           // MSX classic
		{{14, 4, 34}, {170, 40, 120}, {26, 4, 44}, {8, 0, 18}, {255, 70, 210}, 0.85f, false, true},       // synthwave
		{{30, 30, 32}, {16, 16, 17}, {20, 20, 21}, {6, 6, 6}, {240, 120, 50}, 0.6f, false, true},         // Al Alamiah
	};
	const ThemeLook& L = kLooks[std::max(1, std::min(4, theme))];
	constexpr float kWallZ = 2300.0f, kSpacing = 140.0f;
	const float base_y = kHorizon + kFocal * kCamY / kWallZ; // the wall meets the floor here
	pool.Run([&](int x0, int x1) {
		for (int y = 0; y < H; y++)
		{
			uint32_t* row = &bd.px[size_t(y) * W];
			for (int x = x0; x < x1; x++)
			{
				float c[3];
				float g = 0;
				if (y < base_y)
				{
					// the wall: X, Y on a plane kWallZ away
					const float t = std::clamp(float(y) / base_y, 0.0f, 1.0f);
					for (int k = 0; k < 3; k++)
						c[k] = L.wall_top[k] + (L.wall_base[k] - L.wall_top[k]) * t * t;
					if (L.wall_grid)
					{
						const float fp = kWallZ / kFocal * 1.2f;
						const float wx = (x - W * 0.5f) * kWallZ / kFocal, wy = kCamY - (y - kHorizon) * kWallZ / kFocal;
						g = std::max(LineAt(wx, kSpacing, fp), LineAt(wy, kSpacing, fp)) * 0.75f;
					}
				}
				else
				{
					// the floor: its depth from the row, X from the column
					const float z = kFocal * kCamY / std::max(0.5f, y - kHorizon);
					const float t = std::clamp((kWallZ - z) / (kWallZ - 400.0f), 0.0f, 1.0f);
					for (int k = 0; k < 3; k++)
						c[k] = L.floor_far[k] + (L.floor_near[k] - L.floor_far[k]) * t;
					if (L.floor_grid)
					{
						const float fx = z / kFocal * 1.2f, fz = z * z / (kFocal * kCamY) * 1.2f;
						const float wx = (x - W * 0.5f) * z / kFocal;
						g = std::max(LineAt(wx, kSpacing, fx), LineAt(z, kSpacing, fz));
						g *= std::clamp(1.4f - z / kWallZ, 0.25f, 1.0f); // dimmer far away
					}
				}
				// the line where wall and floor meet: a soft glow
				const float joint = std::max(0.0f, 1.0f - std::abs(y - base_y) / 10.0f);
				g = std::max(g, joint * 0.9f);
				int ch[3];
				const float a = std::min(1.0f, L.line_k * g); // the lines laid over the colour, as on the top picture
				for (int k = 0; k < 3; k++)
					ch[k] = int(c[k] * (1 - a) + L.line[k] * a);
				row[x] = Rgb(uint8_t(std::min(255, ch[0])), uint8_t(std::min(255, ch[1])), uint8_t(std::min(255, ch[2])));
			}
		}
	});
	if (theme == 2)
	{
		// MSX classic: plain (the logo is on the top screen)
	}
	else if (theme == 3)
	{
		// synthwave: the sun on the horizon, yellow to pink, cut by bands that thicken towards the bottom
		const float cx = W * 0.5f, cy = base_y, r = 330;
		for (int y = int(cy - r); y < int(cy); y++)
		{
			const float t = (y - (cy - r)) / r; // 0 top .. 1 horizon
			const float band = std::fmod(cy - y, 46.0f);
			if (t > 0.45f && band < 6.0f + 18.0f * (t - 0.45f) / 0.55f)
				continue;
			const float half = std::sqrt(std::max(0.0f, r * r - (y - cy) * (y - cy)));
			const uint8_t R = 255, G = uint8_t(230 - 170 * t), B = uint8_t(60 + 120 * t);
			for (int x = int(cx - half); x < int(cx + half); x++)
				if (x >= 0 && x < W && y >= 0)
					bd.px[size_t(y) * W + x] = Rgb(R, G, B);
		}
	}
	else if (theme == 4)
	{
		// Al Alamiah: the grey band of the boxes across the top, the orange line under it
		for (int y = 0; y < 34; y++)
		{
			const uint8_t v = uint8_t(150 - y);
			for (int x = 0; x < W; x++)
				bd.px[size_t(y) * W + x] = y < 26 ? Rgb(v, v, uint8_t(v - 2)) : Rgb(226, 98, 43);
		}
	}
}

#if defined(MSX_ANDROID)
// The top screen with Cover glow: the cover's glow over the whole screen and the game's main screenshot in the middle,
// at the MSX's 4:3, on a soft shadow with a thin light edge.
void SendTopPicture(Pool& pool, const CoverTex* tex, const GameInfo* g)
{
	Backdrop top;
	BuildBackdrop(pool, top, tex, true);
	std::vector<uint32_t> shot;
	int sw = 0, sh = 0;
	if (g && LoadMainScreenshot(*g, shot, sw, sh) && sw > 0 && sh > 0)
	{
		constexpr int kH = 840, kW = kH * 4 / 3, x0 = (W - kW) / 2, y0 = (H - kH) / 2;
		// the shadow
		for (int y = y0 - 30; y < y0 + kH + 40; y++)
			for (int x = x0 - 30; x < x0 + kW + 30; x++)
			{
				if (y < 0 || y >= H || x < 0 || x >= W)
					continue;
				const float dx = std::max({0.0f, float(x0 - x), float(x - (x0 + kW))});
				const float dy = std::max({0.0f, float(y0 + 10 - y), float(y - (y0 + kH + 10))});
				const float k = std::max(0.0f, 1.0f - std::sqrt(dx * dx + dy * dy) / 30.0f) * 0.6f;
				uint32_t& p = top.px[size_t(y) * W + x];
				p = Rgb(uint8_t((p & 0xff) * (1 - k)), uint8_t(((p >> 8) & 0xff) * (1 - k)), uint8_t(((p >> 16) & 0xff) * (1 - k)));
			}
		// the picture, bilinear
		pool.Run([&](int c0, int c1) {
			for (int y = 0; y < kH; y++)
			{
				const float fy = (y + 0.5f) * sh / kH - 0.5f;
				const int ya = std::clamp(int(std::floor(fy)), 0, sh - 1), yb = std::min(ya + 1, sh - 1);
				const float wy = std::clamp(fy - ya, 0.0f, 1.0f);
				for (int x = std::max(0, c0 - x0); x < std::min(kW, c1 - x0); x++)
				{
					const float fx = (x + 0.5f) * sw / kW - 0.5f;
					const int xa = std::clamp(int(std::floor(fx)), 0, sw - 1), xb = std::min(xa + 1, sw - 1);
					const float wx = std::clamp(fx - xa, 0.0f, 1.0f);
					int ch[3];
					for (int c = 0; c < 3; c++)
					{
						auto at = [&](int xx, int yy) { return float((shot[size_t(yy) * sw + xx] >> (8 * c)) & 0xff); };
						const float t = at(xa, ya) * (1 - wx) + at(xb, ya) * wx, b = at(xa, yb) * (1 - wx) + at(xb, yb) * wx;
						ch[c] = int(t * (1 - wy) + b * wy);
					}
					top.px[size_t(y0 + y) * W + x0 + x] = Rgb(uint8_t(ch[0]), uint8_t(ch[1]), uint8_t(ch[2]));
				}
			}
		});
		// the edge
		const uint32_t edge = Rgb(200, 200, 215);
		for (int i = -3; i < 0; i++)
		{
			for (int x = x0 + i; x < x0 + kW - i; x++)
				top.px[size_t(y0 + i) * W + x] = edge, top.px[size_t(y0 + kH - 1 - i) * W + x] = edge;
			for (int y = y0 + i; y < y0 + kH - i; y++)
				top.px[size_t(y) * W + x0 + i] = edge, top.px[size_t(y) * W + x0 + kW - 1 - i] = edge;
		}
	}
	msx_android_top_picture(top.px.data());
}
#endif

// ---- one cover in 3D ------------------------------------------------------------------------------------
struct Placed
{
	const CoverTex* tex = nullptr;
	float d = 0; // offset from the selection (fractional while gliding)
	float cx = 0, dist = kCamDist; // centre: x, distance from the eye
	float a = 0, b = 0; // x(t) = cx + a*t, dist(t) = dist + b*t, t in [-0.5, 0.5]
	float wh = kCoverH; // world height
	uint32_t bright = 256; // 0..256
	uint32_t alpha = 256; // fade at the ends of the shelf
	int sx0 = 0, sx1 = 0; // screen columns
	float yaw = 0; // the 3D box's turn (facing the middle)
	CoverPtr hold; // the cover (null: the loading card)
};

bool Place(Placed& p, const CoverTex* tex, float d)
{
	p.tex = tex;
	p.d = d;
	const float ad = std::abs(d), sgn = d < 0 ? -1.0f : 1.0f;
	const float k = std::min(ad, 1.0f);
	const float e = k * k * (3 - 2 * k); // smoothstep
	const float x = ad <= 1 ? sgn * kSideX * e : sgn * (kSideX + (ad - 1) * kSideStep);
	const float z = kSideZ * e;
	const float ang = kSideAngle * e;
	p.yaw = sgn * ang;
	const float aspect = float(tex->w[0]) / float(tex->h[0]);
	const float ww = kCoverH * aspect;
	p.cx = x;
	p.dist = kCamDist + z;
	p.a = ww * std::cos(ang);
	// the end towards the middle stands further back: t = +0.5 for a left cover, -0.5 for a right one
	p.b = (d < 0 ? 1.0f : -1.0f) * ww * std::sin(ang);
	p.wh = kCoverH;
	float br = 1.0f - 0.40f * e - 0.07f * std::max(0.0f, ad - 1);
	p.bright = uint32_t(std::clamp(br, 0.25f, 1.0f) * 256);
	p.alpha = uint32_t(std::clamp((kVisible - ad) / 1.0f, 0.0f, 1.0f) * 256);
	// screen extent
	float xs[2];
	for (int i = 0; i < 2; i++)
	{
		const float t = i == 0 ? -0.5f : 0.5f;
		const float dd = p.dist + p.b * t;
		if (dd < 50)
			return false;
		xs[i] = W * 0.5f + kFocal * (p.cx + p.a * t) / dd;
	}
	p.sx0 = int(std::floor(std::min(xs[0], xs[1])));
	p.sx1 = int(std::ceil(std::max(xs[0], xs[1])));
	return p.sx1 > -250 && p.sx0 < W + 250 && p.alpha > 0; // (a 3D object reaches further than the flat cover)
}

// Draws columns [x0, x1) of one placed cover: the cover itself and its reflection on the floor.
void DrawCover(const Placed& p, uint32_t* surf, int x0, int x1)
{
	const CoverTex& t = *p.tex;
	const int cx0 = std::max(x0, p.sx0), cx1 = std::min(x1, p.sx1);
	for (int x = cx0; x < cx1; x++)
	{
		const float X = (x + 0.5f - W * 0.5f) / kFocal;
		const float den = p.a - X * p.b;
		if (std::abs(den) < 1e-4f)
			continue;
		const float tt = (X * p.dist - p.cx) / den; // -0.5 .. 0.5
		if (tt < -0.5f || tt > 0.5f)
			continue;
		const float dd = p.dist + p.b * tt;
		const float top = kHorizon - kFocal * (p.wh - kCamY) / dd;
		const float bot = kHorizon + kFocal * kCamY / dd;
		const float span = bot - top;
		// mip level from the vertical scale
		int l = 0;
		const float scale = span / float(t.h[0]);
		if (scale < 0.38f)
			l = 2;
		else if (scale < 0.75f)
			l = 1;
		const int tw = t.w[l], th = t.h[l];
		const uint32_t* px = t.px[l].data();
		const float u = (tt + 0.5f) * tw - 0.5f;
		const int u0 = std::clamp(int(std::floor(u)), 0, tw - 1), u1 = std::min(u0 + 1, tw - 1);
		const uint32_t fu = uint32_t(std::clamp(u - std::floor(u), 0.0f, 1.0f) * 256);
		const uint32_t* c0 = px + size_t(u0) * th;
		const uint32_t* c1 = px + size_t(u1) * th;
		const float dv = th / span;
		// edge columns: soften the vertical edges a little
		uint32_t edge = 256;
		if (x == p.sx0 || x == p.sx1 - 1)
			edge = 140;
		const uint32_t bright = p.bright;
		const uint32_t alpha = (p.alpha * edge) >> 8;

		// fixed point: v in 16.16
		const int32_t vmax = (th - 1) << 16;
		auto sample = [&](int32_t v) -> uint32_t {
			v = std::clamp(v, 0, vmax - 1);
			const int y0 = v >> 16;
			const uint32_t fv = uint32_t(v >> 8) & 0xff;
			const uint32_t a0 = Lerp32(c0[y0], c0[y0 + 1], fv);
			const uint32_t a1 = Lerp32(c1[y0], c1[y0 + 1], fv);
			return Lerp32(a0, a1, fu);
		};
		const int32_t dv16 = int32_t(dv * 65536.0f);

		// the cover: full rows in the middle, partial coverage on the first and last row
		const int ytop = std::max(0, int(std::floor(top)));
		const int ybot = std::min(H - 1, int(std::ceil(bot)) - 1);
		int32_t v = int32_t(((ytop + 0.5f - top) * dv - 0.5f) * 65536.0f);
		uint32_t* d = surf + size_t(ytop) * W + x;
		for (int y = ytop; y <= ybot; y++, v += dv16, d += W)
		{
			uint32_t a = alpha;
			if (y == ytop || y == ybot)
			{
				float cov = 1.0f;
				if (y < top)
					cov -= top - y;
				if (y + 1 > bot)
					cov -= (y + 1) - bot;
				if (cov <= 0)
					continue;
				a = (alpha * uint32_t(cov * 256)) >> 8;
			}
			const uint32_t c = Scale32(sample(v), bright);
			*d = a >= 256 ? c : Lerp32(*d, c, a);
		}
		// the reflection, fading out, mirrored from the cover's bottom edge up
		const float rlen = span * kReflect;
		const int r0 = std::max(0, int(std::ceil(bot)));
		const int r1 = std::min(H - 1, int(bot + rlen));
		const float inv_rlen = 1.0f / rlen;
		int32_t rv = int32_t((float(th - 1) - (r0 + 0.5f - bot) * dv) * 65536.0f);
		uint32_t* rd = surf + size_t(r0) * W + x;
		for (int y = r0; y <= r1; y++, rv -= dv16, rd += W)
		{
			const float k = (y + 0.5f - bot) * inv_rlen;
			if (k >= 1)
				break;
			const uint32_t a = uint32_t(0.30f * (1 - k) * (1 - k) * alpha);
			if (a == 0)
				continue;
			*rd = Lerp32(*rd, Scale32(sample(std::max(0, rv)), bright), a);
		}
	}
}

// ---- the 3D shelf (fe_cart3d) -----------------------------------------------------------------------------
// Each game stands as its box or (Settings: Games on the shelf) as its cartridge / floppy. A game at the side
// (|d| >= 1) only slides sideways - the same picture, moved - so it is drawn once for its side (a sprite) and put on
// screen each frame; only the middle one, and the ones crossing the middle, are drawn in 3D every frame.
constexpr float kCartH = 320.0f, kFloppyH = 430.0f; // the cartridges / floppies on the shelf
constexpr float kWideBoxH = 390.0f;                 // a landscape box
constexpr float kSideBright = 0.60f;                // a side game's brightness one step from the middle
constexpr float kSideFrom = 0.999f;                 // |d| from which a game is a sprite

// style: Settings' shelf_style (0 boxes, 1 cartridges / floppies, 2 floppies)
cart3d::Kind ItemKind(const CoverTex& t, int style)
{
	if (style == 2)
		return cart3d::Kind::Floppy;
	if (style == 1)
		return t.media;
	return cart3d::Kind::Box; // (portrait, whatever the cover: its art is made portrait - fe_covers' Decode)
}
float ItemHeight(cart3d::Kind k)
{
	return k == cart3d::Kind::Box ? kCoverH : k == cart3d::Kind::BoxWide ? kWideBoxH : k == cart3d::Kind::Cartridge ? kCartH : kFloppyH;
}
// a game one step from the middle, on side -1 (left) or 1
cart3d::Pose SidePose(int side, cart3d::Kind k)
{
	cart3d::Pose q;
	q.x = side * kSideX, q.z = kCamDist + kSideZ, q.yaw = side * kSideAngle, q.height = ItemHeight(k), q.bright = kSideBright;
	return q;
}

class SpriteCache
{
public:
	const cart3d::Sprite* Find(const CoverTex* t, int side, cart3d::Kind k, uint64_t frame)
	{
		for (auto& e : m_items)
			if (e->tex.get() == t && e->side == side && e->kind == k)
			{
				e->used = frame;
				return &e->spr;
			}
		return nullptr;
	}
	// Asks for a sprite; Make draws them in the order asked.
	void Want(const CoverPtr& t, int side, cart3d::Kind k, uint64_t frame)
	{
		if (Find(t.get(), side, k, frame))
			return;
		for (const Wanted& w : m_want)
			if (w.tex == t && w.side == side && w.kind == k)
				return;
		m_want.push_back({t, side, k});
	}
	// Draws the wanted sprites (with the pool) for up to `budget` seconds, one at least; the rest wait for the next
	// frame's list. Returns how many were drawn.
	int Make(Pool& pool, const cart3d::View& v, double budget, uint64_t frame)
	{
		const double t0 = Now();
		int made = 0;
		for (const Wanted& w : m_want)
		{
			if (made > 0 && Now() - t0 > budget)
				break;
			auto e = std::make_unique<Entry>();
			e->tex = w.tex, e->side = w.side, e->kind = w.kind, e->used = frame;
			const cart3d::Pose q = SidePose(w.side, w.kind);
			if (cart3d::Prepare(e->spr, v, w.kind, q, w.tex->art ? &*w.tex->art : nullptr))
			{
				cart3d::Sprite& sp = e->spr;
				const cart3d::Art& art = *w.tex->art;
				// the sprite's columns spread over the screen's strips, so every thread gets a share
				pool.Run([&](int x0, int x1) {
					cart3d::Render(sp, v, w.kind, q, art, sp.x0 + int(int64_t(x0) * sp.w / W), sp.x0 + int(int64_t(x1) * sp.w / W));
				});
			}
			m_items.push_back(std::move(e));
			made++;
		}
		m_want.clear();
		if (m_items.size() > kMax)
		{
			// the least recently used go
			std::sort(m_items.begin(), m_items.end(), [](const auto& a, const auto& b) { return a->used > b->used; });
			m_items.resize(kMax);
		}
		return made;
	}
	void Clear()
	{
		m_items.clear();
		m_want.clear();
	}

private:
	struct Entry
	{
		CoverPtr tex;
		int side = 0;
		cart3d::Kind kind = cart3d::Kind::Box;
		cart3d::Sprite spr;
		uint64_t used = 0;
	};
	struct Wanted
	{
		CoverPtr tex;
		int side;
		cart3d::Kind kind;
	};
	static constexpr size_t kMax = 36; // about 1.5 MB each
	std::vector<std::unique_ptr<Entry>> m_items;
	std::vector<Wanted> m_want;
};

// ---- the play animation (Cross): the game goes into the MSX ---------------------------------------------------
struct PlayShot
{
	bool box = false;      // the box (a shelf of boxes): it turns to us, steps back, and sinks away
	cart3d::Kind box_kind = cart3d::Kind::Box;
	cart3d::Pose box_pose;
	cart3d::Pose item;     // the cartridge / floppy
	float slot = 0;        // the slot in the floor: 0..1 open
	float slot_z = 800, slot_hw = 200, slot_hd = 30; // where, its half width and half depth
	float flash = 0;       // the slot lights up as the game goes in
	float dim = 0;         // the rest of the shelf darkens
	float fade = 0;        // to black at the end
	// a floppy: the front of a floppy drive in front of us, the disk sliding into its slot
	float drive = 0;       // 0..1 faded in
	bool drive_over = false; // the disk is going in: the drive's front above the slot is drawn over it
	float drive_z = 1100, drive_y = 120, drive_hw = 260; // the front's depth, the slot's height, its half width
	float led = 0;         // the activity light
	// 2026-10-16: a clamshell box (the Sakhr collection) opens: its lid swings round the hinge, the cartridge in the lid's
	// tray, the manual in the back half; then the cartridge comes out
	bool clam = false;
	cart3d::Pose lid, back_half;
	bool cart_over_lid = false; // the lid's inside faces us: the cartridge in its tray is drawn after it
	// 2026-10-16: a normal box: its insert (a moulded tray holding the cartridge) slides down out of it, then the
	// cartridge comes out
	bool ins = false;
	cart3d::Pose insert;
	bool cart_out = false; // the cartridge has left the tray (drawn last, in front of everything)
};
inline float Ease(float a)
{
	a = std::clamp(a, 0.0f, 1.0f);
	return a * a * (3 - 2 * a);
}
constexpr float kTwoPi = 6.2831853f;
constexpr float kSlotZ = 780.0f, kSlotZCart = 860.0f; // where the slot opens (boxes, cartridges)

constexpr float kFloppyAt[2] = {2.0f, 2.9f}; // when a floppy starts going into the drive (shelf, boxes)
constexpr float kFloppyIn = 1.25f;            // ... and how long it takes until the fade

double PlayTime(bool boxes, cart3d::Kind media, bool clam = false, cart3d::Kind box_kind = cart3d::Kind::BoxWide, bool insert = true)
{
	if (boxes && media == cart3d::Kind::Cartridge && (clam || (insert && box_kind == cart3d::Kind::Box)))
		return 4.55; // (a clamshell opening, or a normal box's insert sliding out)
	if (media == cart3d::Kind::Floppy)
		return kFloppyAt[boxes ? 1 : 0] + kFloppyIn + 0.5;
	return boxes ? 3.8 : 3.3;
}

// A clamshell box open by `open` (0 shut .. 1 flat open, its lid swung round the hinge on the spine's side): its back half
// (the manual inside), its lid (the tray inside) and the cartridge lying in the lid's tray. (cx, cz): where the shut box
// stands, yaw: its turn; open, it is centred on its hinge. over: the lid's inside faces us (the cartridge drawn after it).
void ClamPoses(float open, float cx, float cz, float yaw, cart3d::Pose& back_half, cart3d::Pose& lid, cart3d::Pose& cart, bool& over)
{
	const float bh = ItemHeight(cart3d::Kind::Box), W = bh * 0.72f, dl = bh * 0.085f;
	// (a turn by a, as the renderer's yaw: +x goes to (cos a, -sin a), +z to (sin a, cos a))
	auto turn = [](float a, float lx, float lz, float& wx, float& wz) {
		wx = lx * std::cos(a) + lz * std::sin(a);
		wz = -lx * std::sin(a) + lz * std::cos(a);
	};
	// unturned, about the shut box's middle: the hinge on its left edge (moving to the middle as it opens)
	const float hinge_x = -0.5f * W * (1 - open), hinge_z = 0;
	const float th = 3.14159265f * (1 + open); // the lid's turn: pi shut .. 2 pi flat open
	auto lid_frame = [&](float lx, float lz, float& ux, float& uz) { // the lid's frame -> unturned, about the hinge
		float rx, rz;
		turn(th, lx, lz, rx, rz);
		ux = hinge_x + rx, uz = hinge_z + rz;
	};
	// ... then the whole box turned by yaw about its middle, and stood at (cx, cz)
	auto stand = [&](float ux, float uz, cart3d::Pose& q, float own_yaw) {
		float wx, wz;
		turn(yaw, ux, uz, wx, wz);
		q.x = cx + wx, q.z = cz + wz, q.yaw = own_yaw + yaw;
	};
	back_half.height = lid.height = bh;
	stand(hinge_x + W / 2, hinge_z + dl / 2, back_half, 0);
	float ux, uz;
	lid_frame(-W / 2, dl / 2, ux, uz);
	stand(ux, uz, lid, th);
	over = open > 0.5f;
	const float hc = bh * 0.36f, cd = hc * 0.24f;
	lid_frame(-W / 2, -cd * 0.2f, ux, uz); // (in front of the tray's floor, a little sunk into its well)
	stand(ux, uz, cart, th);
	cart.height = hc, cart.lift = 0.07f * bh;
}

// The animation at t seconds; yaw0: the middle game's turn when Cross was pressed.
// A normal box with its insert slid out sideways by r (0 in .. 1 out), like a Konami box's tray from its sleeve: the box
// moving left as the insert (a moulded tray) slides out to the right, the cartridge lying in the tray's recess.
// (cx, cz): where the box stands, yaw: its turn (everything turned with it).
void InsertPoses(float r, float cx, float cz, float yaw, cart3d::Pose& box, cart3d::Pose& insert, cart3d::Pose& cart)
{
	const float bh = ItemHeight(cart3d::Kind::Box), W = bh * 0.72f, ih = bh * 0.94f;
	auto at = [&](float lx, float lz, cart3d::Pose& q) { // the box's frame (x right, z back) -> the world
		q.x = cx + lx * std::cos(yaw) + lz * std::sin(yaw);
		q.z = cz - lx * std::sin(yaw) + lz * std::cos(yaw);
		q.yaw = yaw;
	};
	const float bx = -0.5f * W * r, ix = 0.5f * W * r * 1.04f; // (a little more: the tray clears the box)
	box.height = bh, box.lift = 0;
	at(bx, 0, box);
	insert.height = ih, insert.lift = 0.03f * bh; // (centred in the box's height)
	at(ix, 0, insert);
	const float hc = ih * 0.36f, cd = hc * 0.24f, dl = ih * 0.085f;
	at(ix, -dl / 2 - cd * 0.2f, cart); // (in front of the tray's floor, a little sunk into its recess)
	cart.height = hc, cart.lift = insert.lift + 0.55f * ih - hc / 2;
}

PlayShot PlayAt(float t, bool boxes, cart3d::Kind box_kind, cart3d::Kind media, float yaw0, bool plain = false, bool clam = false,
	bool insert = true)
{
	if (boxes && !clam && insert && media == cart3d::Kind::Cartridge && box_kind == cart3d::Kind::Box)
	{
		// a normal box: faces us and steps back, its insert slides out sideways (the box moving left, the tray right), we
		// see the cartridge in the tray; the cartridge rises out, comes to us turning once and goes down into the slot
		PlayShot s;
		s.ins = true;
		const float face = Ease(t / 0.7f), slide = Ease((t - 0.8f) / 1.0f), gone = Ease((t - 3.0f) / 0.8f);
		s.dim = 0.6f * face;
		cart3d::Pose& c = s.item;
		InsertPoses(slide, 0, kCamDist + 250 * face + 200 * slide, yaw0 * (1 - face), s.box_pose, s.insert, c);
		for (cart3d::Pose* q : {&s.box_pose, &s.insert})
		{
			q->alpha = 1 - gone;
			q->fast = true;
			q->shadow = false;
		}
		s.box = gone < 1;
		c.shadow = false;
		c.fast = true;
		const float hc = c.height;
		const float lift_out = Ease((t - 2.0f) / 0.6f), fly = Ease((t - 2.6f) / 1.0f), down = Ease((t - 3.6f) / 0.5f);
		s.cart_out = lift_out > 0;
		c.z -= 170 * lift_out, c.lift += 40 * lift_out;
		const float eye = kCamY - hc / 2;
		c.x = c.x * (1 - fly);
		c.z = c.z + (kSlotZ - c.z) * fly;
		const float hover = c.lift + (eye - c.lift) * fly;
		c.lift = hover * (1 - down) - hc * 1.05f * down;
		c.yaw += kTwoPi * Ease((t - 2.6f) / 1.1f);
		c.clip_floor = down > 0 ? 0.0f : -1.0f;
		s.slot = Ease((t - 3.1f) / 0.5f);
		s.slot_z = kSlotZ;
		s.flash = std::max(0.0f, 1 - std::fabs(t - 4.1f) / 0.3f);
		s.fade = Ease((t - 4.05f) / 0.45f);
		float w, d;
		cart3d::Proportions(media, w, d);
		s.slot_hw = hc * w * 0.5f * 1.12f;
		s.slot_hd = std::max(14.0f, hc * d * 0.5f * 1.8f);
		return s;
	}
	if (boxes && clam && media == cart3d::Kind::Cartridge)
	{
		// the clamshell: faces us, its lid swings open round the hinge (the spine's side, on the left), we see the
		// cartridge in the lid's tray and the manual in the back half; the cartridge rises out, comes to us turning once
		// and goes down into the slot; the open box sinks away
		PlayShot s;
		s.clam = true;
		const float bh = ItemHeight(cart3d::Kind::Box), W = bh * 0.72f, dl = bh * 0.085f;
		const float face = Ease(t / 0.7f), open = Ease((t - 0.8f) / 1.0f), sink = Ease((t - 2.9f) / 0.9f);
		s.dim = 0.6f * face;
		const float yaw_box = yaw0 * (1 - face);
		cart3d::Pose& c = s.item;
		ClamPoses(open, 0, kCamDist + 250 * face + 160 * open, yaw_box, s.back_half, s.lid, c, s.cart_over_lid);
		// (cx 0, cz kCamDist + 410 open: where the shelf's middle box stands opened by a swipe)
		for (cart3d::Pose* q : {&s.back_half, &s.lid})
		{
			q->lift = -1.02f * bh * sink;
			q->clip_floor = sink > 0 ? 0.0f : -1.0f;
			q->bright = 1 - 0.3f * sink;
			q->fast = true;
			q->shadow = false;
		}
		c.shadow = false;
		c.fast = true;
		const float hc = c.height;
		(void)W, (void)dl;
		const float lift_out = Ease((t - 2.0f) / 0.6f), fly = Ease((t - 2.6f) / 1.0f), down = Ease((t - 3.6f) / 0.5f);
		c.z -= 170 * lift_out, c.lift += 40 * lift_out;
		const float eye = kCamY - hc / 2;
		c.x = c.x * (1 - fly);
		c.z = c.z + (kSlotZ - c.z) * fly;
		const float hover = c.lift + (eye - c.lift) * fly;
		c.lift = hover * (1 - down) - hc * 1.05f * down;
		c.yaw += kTwoPi * Ease((t - 2.6f) / 1.1f); // (from the lid's turn)
		c.clip_floor = down > 0 ? 0.0f : -1.0f;
		s.slot = Ease((t - 3.1f) / 0.5f);
		s.slot_z = kSlotZ;
		s.flash = std::max(0.0f, 1 - std::fabs(t - 4.1f) / 0.3f);
		s.fade = Ease((t - 4.05f) / 0.45f);
		float w, d;
		cart3d::Proportions(media, w, d);
		s.slot_hw = hc * w * 0.5f * 1.12f;
		s.slot_hd = std::max(14.0f, hc * d * 0.5f * 1.8f);
		return s;
	}
	const float floppy_at = kFloppyAt[boxes ? 1 : 0];
	if (media == cart3d::Kind::Floppy && !plain && t > floppy_at - 0.8f)
	{
		// a floppy: as before until it hovers in front of us, then into the drive that fades in
		PlayShot s = PlayAt(std::min(t, floppy_at), boxes, box_kind, media, yaw0, true);
		const cart3d::Pose from = s.item;
		cart3d::Pose& c = s.item;
		const float u = t - floppy_at;
		const float tilt = Ease(u / 0.55f), slide = Ease((u - 0.55f) / 0.6f);
		s.slot = 0; // no slot in the floor
		s.drive = Ease((t - (floppy_at - 0.8f)) / 0.6f);
		s.drive_hw = s.slot_hw * 1.3f;
		s.drive_y = kCamY - 110; // below the eye: we look down onto the disk
		const float h = c.height;
		const float z_front = s.drive_z - h - 25; // flat, its far edge (the shutter) just short of the slot
		if (u > 0)
		{
			// tips back flat around its bottom edge: the label up, the shutter away
			c.pitch = 1.5708f * tilt;
			c.yaw = 0;
			c.x = 0;
			c.lift = from.lift + (s.drive_y - from.lift) * tilt;
			c.z = from.z + (z_front - from.z) * tilt + (h + 40) * slide;
			c.clip_floor = -1;
			c.reflection = false;
			s.drive_over = slide > 0;
		}
		s.led = std::max(0.0f, 1 - std::fabs(u - 1.05f) / 0.35f);
		s.flash = 0;
		s.fade = Ease((u - kFloppyIn) / 0.45f);
		return s;
	}
	PlayShot s;
	s.box_kind = box_kind;
	const float face = Ease(t / 0.7f); // turning to face us
	s.dim = 0.6f * face;
	cart3d::Pose& c = s.item;
	c.shadow = false;
	c.fast = true;
	if (boxes)
	{
		const float bh = ItemHeight(box_kind), hc = kCoverH * (media == cart3d::Kind::Floppy ? 0.47f : 0.36f);
		// the box: faces us and steps back (room above it for the cartridge), later sinks into the floor
		const float sink = Ease((t - 1.9f) / 0.9f);
		s.box = sink < 1;
		s.box_pose.height = bh;
		s.box_pose.z = kCamDist + 250 * face;
		s.box_pose.yaw = yaw0 * (1 - face);
		s.box_pose.lift = -1.02f * bh * sink;
		s.box_pose.clip_floor = sink > 0 ? 0.0f : -1.0f;
		s.box_pose.bright = 1 - 0.3f * sink;
		s.box_pose.fast = true;
		// the cartridge: up out of the box's top (slowly), to us at eye level turning round once, down into the slot
		const float out = Ease((t - 0.6f) / 1.1f), fly = Ease((t - 1.8f) / 1.0f), down = Ease((t - 2.9f) / 0.55f);
		c.height = hc;
		c.z = s.box_pose.z + (kSlotZ - s.box_pose.z) * fly;
		const float top = bh - hc + (hc + 25) * out, eye = kCamY - hc / 2;
		const float hover = top + (eye - top) * fly;
		c.lift = hover * (1 - down) - hc * 1.05f * down;
		c.yaw = s.box_pose.yaw + kTwoPi * Ease((t - 1.8f) / 1.1f);
		c.clip_floor = t < 1.75f ? bh : (down > 0 ? 0.0f : -1.0f); // the box hides it; then the slot swallows it
		s.slot = Ease((t - 2.4f) / 0.5f);
		s.slot_z = kSlotZ;
		s.flash = std::max(0.0f, 1 - std::fabs(t - 3.4f) / 0.3f);
		s.fade = Ease((t - 3.35f) / 0.45f);
	}
	else
	{
		// the cartridge itself: comes to us turning round once, then down into the slot
		const float ih = ItemHeight(media);
		const float rise = Ease((t - 0.3f) / 1.4f), down = Ease((t - 2.1f) / 0.6f);
		c.height = ih;
		c.z = kCamDist + (kSlotZCart - kCamDist) * rise;
		const float hover = std::max(0.0f, kCamY - ih / 2) * rise;
		c.lift = hover * (1 - down) - ih * 1.05f * down;
		c.yaw = yaw0 * (1 - face) + kTwoPi * Ease((t - 0.4f) / 1.4f);
		c.clip_floor = down > 0 ? 0.0f : -1.0f;
		s.slot = Ease((t - 1.6f) / 0.5f);
		s.slot_z = kSlotZCart;
		s.flash = std::max(0.0f, 1 - std::fabs(t - 2.75f) / 0.3f);
		s.fade = Ease((t - 2.85f) / 0.45f);
	}
	float w, d;
	cart3d::Proportions(media, w, d);
	s.slot_hw = c.height * w * 0.5f * 1.12f;
	s.slot_hd = std::max(14.0f, c.height * d * 0.5f * 1.8f);
	return s;
}

// The middle cartridge turned to show its connector (the microphone on): e 0..1 the way there; yaw its turn;
// from_box: it comes out of its box / floppy (fading in) rather than being the shelf's own cartridge.
cart3d::Pose CircuitPose(float e, float yaw, bool from_box)
{
	cart3d::Pose q;
	const float k = Ease(e);
	q.height = kCartH + (420 - kCartH) * k;
	q.z = kCamDist + (780 - kCamDist) * k;
	q.lift = (from_box ? kCoverH * 0.25f : 0.0f) * (1 - k) + 250 * k;
	q.pitch = 0.62f * k; // label up, the connector towards us
	q.yaw = yaw;
	q.alpha = from_box ? std::min(1.0f, e * 3) : 1.0f;
	q.shadow = k < 0.3f;
	return q;
}

// Dust blown out of the connector, as smoke: soft puffs in screen space (size: the radius), each with its own corner
// of the noise that makes it wispy.
struct Dust
{
	float x, y, vx, vy, age, life, size;
	uint32_t color;
	float nu, nv; // where its puff reads the noise
};

// A tiling 128 x 128 noise (three octaves of smooth value noise), 0..255: the smoke's wisps.
const uint8_t* SmokeNoise()
{
	static uint8_t* tex = nullptr;
	if (tex)
		return tex;
	tex = new uint8_t[128 * 128];
	uint32_t seed = 0x5eed1234u;
	auto rnd = [&] {
		seed = seed * 1664525u + 1013904223u;
		return float(seed >> 8) / 16777216.0f;
	};
	float lattice[3][32][32];
	for (auto& o : lattice)
		for (auto& row : o)
			for (float& v : row)
				v = rnd();
	for (int y = 0; y < 128; y++)
		for (int x = 0; x < 128; x++)
		{
			float sum = 0, amp = 0.55f;
			for (int o = 0; o < 3; o++)
			{
				const int cells = 4 << o; // 4, 8, 16 cells across
				const float fx = x * cells / 128.0f, fy = y * cells / 128.0f;
				const int x0 = int(fx) % cells, y0 = int(fy) % cells, x1 = (x0 + 1) % cells, y1 = (y0 + 1) % cells;
				float tx = fx - std::floor(fx), ty = fy - std::floor(fy);
				tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty);
				const float a = lattice[o][y0][x0] + (lattice[o][y0][x1] - lattice[o][y0][x0]) * tx;
				const float b = lattice[o][y1][x0] + (lattice[o][y1][x1] - lattice[o][y1][x0]) * tx;
				sum += (a + (b - a) * ty) * amp;
				amp *= 0.5f;
			}
			tex[y * 128 + x] = uint8_t(std::clamp(sum / 0.9625f, 0.0f, 1.0f) * 255);
		}
	return tex;
}

// Columns [x0, x1) of the slot in the floor: a dark opening with lit lips, in a soft glow.
void DrawSlot(uint32_t* surf, int x0, int x1, const PlayShot& s)
{
	if (s.slot <= 0)
		return;
	const float k = s.slot;
	const float gw = s.slot_hw * 1.8f, gd = s.slot_hd * 8;
	auto row_of = [](float z) { return kHorizon + kFocal * kCamY / z; };
	const int ya = std::max(0, int(row_of(s.slot_z + gd))), yb = std::min(H - 1, int(row_of(std::max(80.0f, s.slot_z - gd))) + 1);
	for (int y = ya; y <= yb; y++)
	{
		const float zz = kFocal * kCamY / std::max(0.5f, y + 0.5f - kHorizon); // the floor's depth on this row
		const float dz = (zz - s.slot_z) / gd;
		if (std::fabs(dz) > 1)
			continue;
		const float ppw = kFocal / zz; // pixels a world unit, at that depth
		const int xa = std::max(x0, int(W * 0.5f - gw * ppw)), xb = std::min(x1, int(W * 0.5f + gw * ppw) + 1);
		uint32_t* row = surf + size_t(y) * W;
		for (int x = xa; x < xb; x++)
		{
			const float wx = (x + 0.5f - W * 0.5f) / ppw;
			const float gx = wx / gw, r2 = gx * gx + dz * dz;
			uint32_t px = row[x];
			if (r2 < 1)
			{
				const float g = (1 - r2) * (1 - r2) * k * (0.30f + 1.4f * s.flash);
				px = Add32(px, Rgb(uint8_t(std::min(255.0f, 110 * g)), uint8_t(std::min(255.0f, 90 * g)), uint8_t(std::min(255.0f, 230 * g))));
			}
			const float ax = std::fabs(wx), az = zz - s.slot_z;
			if (ax < s.slot_hw && std::fabs(az) < s.slot_hd)
				px = Lerp32(px, Rgb(3, 3, 6), uint32_t(235 * k)); // the opening
			else if (ax < s.slot_hw * 1.04f && az >= s.slot_hd && az < s.slot_hd * 1.7f)
				px = Add32(px, Rgb(uint8_t(150 * k), uint8_t(140 * k), uint8_t(190 * k))); // the far lip, catching the light
			else if (ax < s.slot_hw * 1.04f && az <= -s.slot_hd && az > -s.slot_hd * 1.5f)
				px = Add32(px, Rgb(uint8_t(60 * k), uint8_t(55 * k), uint8_t(80 * k))); // the near lip
			row[x] = px;
		}
	}
}

// Columns [x0, x1) of a floppy drive's front, standing on the floor at drive_z, with the slot at drive_y. upper: only
// the part above the slot's middle (drawn over the disk going in: what is behind the front is hidden); otherwise all
// of it, or (while the disk goes in) the part below.
void DrawDrive(uint32_t* surf, int x0, int x1, const PlayShot& s, bool upper)
{
	if (s.drive <= 0)
		return;
	const float k = s.drive, z = s.drive_z, ppw = kFocal / z;
	auto row_of = [&](float y) { return kHorizon - kFocal * (y - kCamY) / z; };
	const float top_y = s.drive_y + s.drive_hw * 0.32f, slot_h = std::max(6.0f, s.drive_hw * 0.045f);
	const float r_top = row_of(top_y), r_floor = row_of(0), r_slot = row_of(s.drive_y);
	const float r_slot_a = row_of(s.drive_y + slot_h), r_slot_b = row_of(s.drive_y - slot_h);
	const float cx = W * 0.5f, hw = s.drive_hw * ppw, slot_hw = s.drive_hw * 0.80f * ppw;
	int ya = std::max(0, int(r_top)), yb = std::min(H - 1, int(r_floor));
	if (upper)
		yb = std::min(yb, int(r_slot));
	else if (s.drive_over)
		ya = std::max(ya, int(r_slot) + 1);
	const int xa = std::max(x0, int(cx - hw)), xb = std::min(x1, int(cx + hw) + 1);
	const uint32_t a = uint32_t(255 * k);
	for (int y = ya; y <= yb; y++)
	{
		uint32_t* row = surf + size_t(y) * W;
		const float fy = (y + 0.5f - r_top) / std::max(1.0f, r_floor - r_top); // 0 top .. 1 floor
		const bool in_slot_rows = y >= int(r_slot_a) && y <= int(r_slot_b);
		for (int x = xa; x < xb; x++)
		{
			const float dx = x + 0.5f - cx;
			// the front: dark grey plastic, lighter at the top edge, a bevel at the sides
			const float edge = std::min(std::fabs(std::fabs(dx) - hw), (y + 0.5f - r_top));
			uint8_t g = uint8_t(std::clamp(46.0f - 14.0f * fy + (edge < 3 ? 40.0f : 0.0f), 0.0f, 255.0f));
			uint32_t c = Rgb(g, g, uint8_t(std::min(255, g + 6)));
			if (in_slot_rows && std::fabs(dx) < slot_hw)
				c = (y == int(r_slot_b)) ? Rgb(90, 90, 104) : Rgb(4, 4, 7); // the slot, its lower lip catching light
			// the eject button and the activity light, at the right under the slot
			const float bx = dx - slot_hw * 0.72f, by = y + 0.5f - (r_slot_b + (r_floor - r_slot_b) * 0.22f);
			if (std::fabs(bx) < 0.10f * hw && std::fabs(by) < 0.035f * hw)
				c = Rgb(70, 70, 80);
			const float lx = dx + slot_hw * 0.80f;
			if (std::fabs(lx) < 0.03f * hw && std::fabs(by) < 0.015f * hw)
				c = Lerp32(Rgb(20, 60, 25), Rgb(90, 255, 110), uint32_t(255 * s.led));
			row[x] = Lerp32(row[x], c, a);
		}
	}
}

// ---- input: left/right with key repeat that speeds up -------------------------------------------------
struct Repeat
{
	double next = 0, since = 0;
	bool held = false;
	bool Fire(bool down, double now)
	{
		if (!down)
		{
			held = false;
			return false;
		}
		if (!held)
		{
			held = true;
			since = now;
			next = now + 0.32;
			return true;
		}
		if (now >= next)
		{
			next = now + (now - since > 1.4 ? 0.045 : 0.095);
			return true;
		}
		return false;
	}
};

void CenterText(int y, const std::string& s, int scale, uint32_t color)
{
	const std::string fit = FitText(s, scale, W - 200);
	DrawText((W - TextWidth(fit.c_str(), scale)) / 2, y, fit.c_str(), scale, color);
}

bool Confirm(const char* question, const char* detail = nullptr)
{
	std::vector<uint32_t> saved(ps5video::Surface(), ps5video::Surface() + size_t(W) * H);
	ps5input::Poll();
	uint32_t prev = ps5input::Pad(0).buttons;
	ps5video::DarkenRect(0, 0, W, H);
	const int bw = 900, bh = 240, bx = (W - bw) / 2, by = (H - bh) / 2;
	ps5video::FillRect(bx - 3, by - 3, bw + 6, bh + 6, Rgb(150, 125, 255));
	ps5video::FillRect(bx, by, bw, bh, Rgb(26, 24, 48));
	int qs = 5; // (a long question: smaller, to stay in the box)
	while (qs > 2 && TextWidth(question, qs) > bw - 100)
		qs--;
	DrawText(bx + 50, by + (detail ? 34 : 50), question, qs, Rgb(240, 240, 250));
	if (detail) // MSX PS5 1.2: what the question is about (a game's name), smaller
		DrawText(bx + 50, by + 108, FitText(detail, 3, bw - 100).c_str(), 3, Rgb(255, 210, 110));
	DrawText(bx + 50, by + 160, (std::string(icon::Cross) + " Yes       " + icon::Circle + " No").c_str(), 3, Rgb(170, 160, 210));
	ps5video::Present(0, 0, 0, 0, true);
	for (;;)
	{
		ps5input::Poll();
		const uint32_t cur = ps5input::Pad(0).buttons;
		const uint32_t down = cur & ~prev;
		prev = cur;
		if (down & SCE_PAD_BUTTON_CROSS)
			return true;
		if (down & (SCE_PAD_BUTTON_CIRCLE | SCE_PAD_BUTTON_OPTIONS))
		{
			memcpy(ps5video::Surface(), saved.data(), saved.size() * 4);
			return false;
		}
		ps5video::Present(0, 0, 1, 1, true);
	}
}

// ---- state kept between visits to the shelf (the game list, its covers) -----------------------------------
struct ShelfState
{
	std::vector<GameInfo> all; // the whole library
	int family = -1; // the tab the list below shows
	std::vector<GameInfo> games; // the games on the shelf: the tab's
	CoverService covers;
	std::vector<CoverPtr> slots;
	bool started = false;
	bool downloads = true;
	bool one_version = true; // the list below has one version of each game
};
ShelfState& S()
{
	static ShelfState* s = new ShelfState(); // never destroyed: the cover thread may outlive main's statics
	return *s;
}

std::shared_ptr<CoverTex> LoadingCard()
{
	static std::shared_ptr<CoverTex> card;
	if (card)
		return card;
	const int w = 716, h = kCoverTexH;
	auto tex = std::make_shared<CoverTex>();
	int lw = w, lh = h;
	for (int l = 0; l < CoverTex::kLevels; l++)
	{
		tex->w[l] = lw;
		tex->h[l] = lh;
		tex->px[l].resize(size_t(lw) * lh);
		for (int x = 0; x < lw; x++)
			for (int y = 0; y < lh; y++)
			{
				const bool border = x < 3 || y < 3 || x >= lw - 3 || y >= lh - 3;
				const float t = float(y) / lh;
				tex->px[l][size_t(x) * lh + y] = border ? Rgb(80, 70, 130) : Rgb(uint8_t(36 - 14 * t), uint8_t(32 - 12 * t), uint8_t(64 - 26 * t));
			}
		lw /= 2;
		lh /= 2;
	}
	tex->average = Rgb(60, 50, 110);
	card = tex;
	return card;
}

// ---- favourites (Circle): by file name, so a game stays one when its drive changes ----------------------------
constexpr Family kFavoritesTab = Family(100); // the tab after "All games" (not a family of systems)
constexpr Family kSakhrTab = Family(101);     // 2026-10-16: the Arabic MSX collection (Al Alamiah, Barq, Al-Mithali) on its own
constexpr Family kDriveTab = Family(102);     // MSX PS5 1.2 (2026-10-22, user request): the floppy drive's games on their own
bool OnFloppy(const GameInfo& g)
{
	return IsFloppyFile(g.path) || IsFloppyDevice(g.path);
}

std::string FavoriteKey(const GameInfo& g)
{
	return g.path.substr(g.path.find_last_of('/') + 1);
}
std::string FavoritesPath()
{
	return OrbisRoot() + "/favorites.txt";
}
std::set<std::string>& Favorites()
{
	static std::set<std::string>* f = nullptr;
	if (!f)
	{
		f = new std::set<std::string>();
		if (FILE* in = fopen(FavoritesPath().c_str(), "r"))
		{
			char line[1024];
			while (fgets(line, sizeof(line), in))
			{
				std::string l(line);
				while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
					l.pop_back();
				if (!l.empty() && l[0] != '#')
					f->insert(l);
			}
			fclose(in);
		}
	}
	return *f;
}
bool IsFavorite(const GameInfo& g)
{
	return Favorites().count(FavoriteKey(g)) != 0;
}
void SaveFavorites()
{
	if (FILE* out = fopen(FavoritesPath().c_str(), "w"))
	{
		fprintf(out, "# MSX PS5: your favourite games (Circle on the shelf), by file name\n");
		for (const std::string& e : Favorites())
			fprintf(out, "%s\n", e.c_str());
		fclose(out);
	}
}
// Adds or removes game g; true: it is a favourite now.
bool ToggleFavorite(const GameInfo& g)
{
	std::set<std::string>& f = Favorites();
	const std::string k = FavoriteKey(g);
	const bool now = !f.count(k);
	if (now)
		f.insert(k);
	else
		f.erase(k);
	SaveFavorites();
	OrbisLog("[shelf] favourite %s: %s", now ? "added" : "removed", k.c_str());
	return now;
}
const char* TabName(Family f)
{
	return f == kFavoritesTab ? "Favorites" : f == kSakhrTab ? "Sakhr" : f == kDriveTab ? "Floppy drive" : FamilyName(f);
}

// A filled five-pointed star (the favourite's mark), centred at (cx, cy).
void DrawStar(float cx, float cy, float r, uint32_t color)
{
	float px[10], py[10];
	for (int i = 0; i < 10; i++)
	{
		const float a = -1.5707963f + i * 0.6283185f, rr = i % 2 ? r * 0.42f : r;
		px[i] = cx + rr * std::cos(a), py[i] = cy + rr * std::sin(a);
	}
	uint32_t* surf = ps5video::Surface();
	for (int y = std::max(0, int(cy - r)); y <= std::min(H - 1, int(cy + r)); y++)
		for (int x = std::max(0, int(cx - r)); x <= std::min(W - 1, int(cx + r)); x++)
		{
			// 4 samples a pixel (soft edges): even-odd rule
			int in = 0;
			for (int k = 0; k < 4; k++)
			{
				const float fx = x + 0.25f + 0.5f * (k & 1), fy = y + 0.25f + 0.5f * (k >> 1);
				bool inside = false;
				for (int i = 0, j = 9; i < 10; j = i++)
					if ((py[i] > fy) != (py[j] > fy) && fx < (px[j] - px[i]) * (fy - py[i]) / (py[j] - py[i]) + px[i])
						inside = !inside;
				in += inside;
			}
			if (in)
				surf[size_t(y) * W + x] = Lerp32(surf[size_t(y) * W + x], color, uint32_t(in * 64));
		}
}

// The tabs that have games: "All games" first, then Favorites (if any), then each family present in the library.
std::vector<Family> Tabs(const std::vector<GameInfo>& all)
{
	std::vector<Family> tabs = {Family::All};
	for (const GameInfo& g : all)
		if (OnFloppy(g))
		{
			tabs.push_back(kDriveTab); // (MSX PS5 1.2: right after All games, while a floppy is in)
			break;
		}
	if (!Favorites().empty())
		tabs.push_back(kFavoritesTab);
	for (const GameInfo& g : all)
		if (IsArabicEdition(g))
		{
			tabs.push_back(kSakhrTab);
			break;
		}
	for (int f = 1; f < int(Family::Count); f++)
		for (const GameInfo& g : all)
			if (Info(g.system).family == Family(f) && !IsArabicEdition(g) && !OnFloppy(g))
			{
				tabs.push_back(Family(f));
				break;
			}
	return tabs;
}

std::vector<GameInfo> InFamily(const std::vector<GameInfo>& all, Family f, bool one_version)
{
	std::vector<GameInfo> out;
	if (f == kFavoritesTab)
	{
		// every version you marked, but each file once: the same file on the internal storage and on a drive (the user:
		// duplicates in Favorites, after the games were copied from the RetroBat drive) - the first found, internal
		std::set<std::string> files;
		for (const GameInfo& g : all)
		{
			if (!IsFavorite(g))
				continue;
			std::string name = g.path.substr(g.path.find_last_of('/') + 1);
			for (char& c : name)
				c = char(tolower(uint8_t(c)));
			if (files.insert(name).second)
				out.push_back(g);
		}
		return out;
	}
	// the Sakhr collection: on its own tab only (not among "All games" and the systems' tabs); so is the floppy drive's
	for (const GameInfo& g : all)
		if (f == kDriveTab ? OnFloppy(g)
			: !OnFloppy(g) && (f == kSakhrTab ? IsArabicEdition(g) : !IsArabicEdition(g) && (f == Family::All || Info(g.system).family == f)))
			out.push_back(g);
	return one_version ? OneOfEach(out) : out;
}

void Loading(const char* text)
{
	ps5video::FillRect(0, 0, W, H, Rgb(12, 10, 26));
	DrawText(80, 50, "MSX PS5", 6, Rgb(150, 125, 255));
	CenterText(H / 2 - 20, text, 4, Rgb(220, 220, 235));
	ps5video::Present(0, 0, 0, 0, true);
}
} // namespace

void ShelfShutdown()
{
	S().covers.Stop();
}

// MSX PS5 1.2 (2026-10-22, user request: "if I insert a real floppy, add an option to copy it to the MSX collection, and if a
// copy exists show a pop up to overwrite it"): Square on the Floppy drive tab. A game file on a PC floppy goes to
// <data>/roms/MSX; when the collection (the console's own, never a USB drive's) already has the game -- the same file name,
// or the same title ("Penguin Adventure.rom" and "Penguin Adventure (Japan, Europe).zip") -- the player is asked first, and
// the new file takes the old one's name (its cover, its favourite star) and replaces it. An MSX disk of its own (the whole
// floppy) goes to <data>/roms/Disks as "Floppy disk <its hash>.dsk", the name the drive's disk already plays under.
// Returns what to show; `changed`: the library is different now (the shelf rescans).
std::string AddFloppyToCollection(const GameInfo& g, const std::vector<GameInfo>& all, bool* changed)
{
	*changed = false;
	auto bare = [](const std::string& n) {
		std::string out;
		int depth = 0;
		for (char c : n)
		{
			if (c == '(' || c == '[')
				depth++;
			else if ((c == ')' || c == ']') && depth > 0)
				depth--;
			else if (depth == 0 && isalnum(uint8_t(c)))
				out += char(tolower(uint8_t(c)));
		}
		return out;
	};
	const std::string own = OrbisDir("roms");
	std::string src_dev, name, dest, existing;
	unsigned cluster = 0, size = 0;
	if (IsFloppyDevice(g.path))
	{
		const std::string tmp = OrbisDir("tmp") + "/floppy-copy.dsk";
		unsigned long long hash = 0;
		Loading("Reading the floppy...");
		const int bad = fe_floppy_read(g.path.c_str(), tmp.c_str(), &hash);
		if (bad < 0)
			return "The floppy couldn't be read";
		char file[64];
		snprintf(file, sizeof(file), "Floppy disk %08llx.dsk", hash & 0xffffffffull);
		OrbisMkdirs(own + "/Disks");
		dest = own + "/Disks/" + file;
		if (OrbisIsFile(dest) && !Confirm("Already in your collection. Overwrite it?", file))
		{
			remove(tmp.c_str());
			return "";
		}
		if (rename(tmp.c_str(), dest.c_str()) != 0)
		{
			remove(tmp.c_str());
			return "Couldn't save the disk in your collection";
		}
		*changed = true;
		OrbisLog("[shelf] floppy %s -> %s (%d unreadable sector(s))", g.path.c_str(), dest.c_str(), bad);
		return bad > 0 ? "Added (some sectors couldn't be read)" : "Added to your collection";
	}
	if (!ParseFloppyFile(g.path, &src_dev, &cluster, &size, &name))
		return "";
	const std::string file = name.substr(name.find_last_of('/') + 1);
	const size_t dot = file.find_last_of('.');
	const std::string ext = dot == std::string::npos ? "" : file.substr(dot);
	// the same game in the console's own collection: by file name, else by title
	const std::string want = bare(g.file_base);
	for (const GameInfo& lg : all)
	{
		if (lg.path.rfind(own + "/", 0) != 0 || IsFloppyFile(lg.path) || IsFloppyDevice(lg.path))
			continue;
		const std::string lfile = lg.path.substr(lg.path.find_last_of('/') + 1);
		if (strcasecmp(lfile.c_str(), file.c_str()) == 0)
		{
			existing = lg.path;
			break;
		}
		if (existing.empty() && !want.empty() && (bare(lg.nointro) == want || bare(lg.file_base) == want))
			existing = lg.path;
	}
	if (!existing.empty())
	{
		// the new file under the old one's name (same folder; the floppy's extension)
		const size_t slash = existing.find_last_of('/'), edot = existing.find_last_of('.');
		const std::string base = edot != std::string::npos && edot > slash ? existing.substr(0, edot) : existing;
		dest = base + ext;
		if (!Confirm("Already in your collection. Overwrite it?", existing.substr(slash + 1).c_str()))
			return "";
	}
	else
	{
		OrbisMkdirs(own + "/MSX");
		dest = own + "/MSX/" + file;
	}
	Loading("Copying from the floppy...");
	const std::string part = dest + ".part";
	if (fe_floppy_extract(src_dev.c_str(), cluster, size, part.c_str()) != 0)
	{
		remove(part.c_str());
		return "The floppy couldn't be read";
	}
	if (!existing.empty() && existing != dest)
	{
		// the old file goes (a .zip replaced by the floppy's .rom); its favourite star moves to the new name
		remove(existing.c_str());
		std::set<std::string>& f = Favorites();
		const std::string oldk = existing.substr(existing.find_last_of('/') + 1), newk = dest.substr(dest.find_last_of('/') + 1);
		if (f.erase(oldk))
		{
			f.insert(newk);
			SaveFavorites();
		}
	}
	if (rename(part.c_str(), dest.c_str()) != 0)
	{
		remove(part.c_str());
		return "Couldn't save the game in your collection";
	}
	*changed = true;
	OrbisLog("[shelf] floppy %s -> %s%s", g.path.c_str(), dest.c_str(), existing.empty() ? "" : (" (replaced " + existing + ")").c_str());
	return existing.empty() ? "Added to your collection" : "Your collection's copy was replaced";
}

std::string Shelf()
{
	Settings& cfg = Config();
	ShelfState& st = S();
	GENPLUS_STAGE(Shelf, "pool");
	Pool pool;
	// the controller's motors and triggers are the shelf's while it is up: left still and free behind it
	struct PadEffects
	{
		~PadEffects()
		{
			ps5mic::Stop();
			ps5input::Rumble(0, 0);
			ps5input::TriggersOff();
		}
		// (the microphone too)
	} pad_effects;
	const bool mic = ps5mic::Start(ps5input::UserId(0));
	OrbisLog("[shelf] microphone %s", mic ? "listening" : "not available");
	sound::Init();
	sound::Music(true); // (fades in; off in Settings > Audio)

	GENPLUS_STAGE(Shelf, "scan games");
	Loading("Looking for games...");
	std::vector<GameInfo> games = ScanGames();
	bool same = games.size() == st.all.size();
	for (size_t i = 0; same && i < games.size(); i++)
		same = games[i].path == st.all[i].path;
	if (!ShelfDownloads() && (!same || !st.started))
	{
		// the console: covers come from the prefetch at start (fe_prefetch.h); new ones restart the app
		GENPLUS_STAGE(Shelf, "wanted covers");
		CoversRestartIfNeeded(games, cfg.covers_download, false, Loading);
	}
	st.all = games;
	// MSX PS5 1.2 (2026-10-22, user request: "boot automatically if the drive is connected"): a floppy's game starts at
	// once, without the shelf -- a game file on a PC floppy, or an MSX disk of its own (720 / 360 KB; a PC disk with no game
	// on it stays a shelf entry). Once per disk: the same disk still in after the game (or after Options) shows the shelf;
	// taken out and put back, it starts again. The floppy watcher payload (floppywatch.elf) opens MSX PS5 for it.
	{
		static std::string s_floppy_seen;
		std::string sig;
		const GameInfo* start = nullptr;
		for (const GameInfo& g : games)
			if (IsFloppyFile(g.path) || IsFloppyDevice(g.path))
			{
				sig += g.path + "\n";
				const bool msx_disk = IsFloppyDevice(g.path) && g.title.find("1.44 MB") == std::string::npos;
				if (!start && (IsFloppyFile(g.path) || msx_disk))
					start = &g;
			}
		if (sig != s_floppy_seen)
		{
			s_floppy_seen = sig;
			if (start)
			{
				OrbisLog("[shelf] floppy: starting \"%s\" (%s) without the shelf", start->title.c_str(), start->path.c_str());
				cfg.last_rom = start->path;
				cfg.shelf_family = int(kDriveTab); // (back from the game: the Floppy drive tab)
				cfg.Save();
				return start->path;
			}
		}
	}
	std::vector<Family> tabs = Tabs(st.all);
	int tab = 0;
	for (size_t i = 0; i < tabs.size(); i++)
		if (int(tabs[i]) == cfg.shelf_family)
			tab = int(i);
	if (!same || !st.started || st.downloads != cfg.covers_download || st.family != int(tabs[size_t(tab)]) ||
		st.one_version != cfg.one_version)
	{
		st.family = int(tabs[size_t(tab)]);
		st.one_version = cfg.one_version;
		st.games = InFamily(st.all, tabs[size_t(tab)], cfg.one_version);
		st.slots.assign(games.size(), nullptr);
		st.downloads = cfg.covers_download;
		GENPLUS_STAGE(Shelf, "cover service start");
		st.covers.Start(st.games, cfg.covers_download && ShelfDownloads(), cfg.covers_download && BackgroundCovers());
		st.started = true;
	}
	// MSX PS5 1.2 (2026-10-22, user: "by default it should appear as floppy"): the Floppy drive tab shows its games as
	// floppies, whatever Settings' style (boxes / cartridges / floppies) is for the other tabs
	auto ShelfStyle = [&]() { return tabs[size_t(tab)] == kDriveTab ? 2 : cfg.shelf_style; };
	int n = int(st.games.size());

	int sel = 0;
	for (int i = 0; i < n; i++)
		if (st.games[size_t(i)].path == cfg.last_rom)
			sel = i;
	float pos = float(sel);
	st.covers.SetFocus(sel);

	Backdrop bd_cur, bd_old;
	float fade = 1.0f; // 0..1: old -> cur
	GENPLUS_STAGE(Shelf, "build backdrop");
	int bd_theme = cfg.shelf_theme;
	if (bd_theme > 0)
		BuildThemeBackdrop(pool, bd_cur, bd_theme);
	else
		BuildBackdrop(pool, bd_cur, nullptr);
#if defined(MSX_ANDROID)
	msx_android_set_theme(bd_theme);
	if (bd_theme == 0)
		SendTopPicture(pool, nullptr, nullptr);
#endif
	GENPLUS_STAGE(Shelf, "first frame");
	const CoverTex* bd_wanted = nullptr;
	double sel_since = Now();
	double tab_since = -10.0;

	Repeat rep_l, rep_r, rep_l1, rep_r1;
	ps5input::Poll();
	uint32_t prev = ps5input::Pad(0).buttons;
	double last = Now();
	bool dirty = true;
	int last_status = -1;

	int frames_since_open = 0; // MSX PS5 1.2 (the floppy check above)
	// the play animation (Cross): the game's way into the MSX
	double play_t0 = -1;
	double play_pressed = -1; // when Cross started it (that press doesn't skip the animation)
	float play_sfx_t = -1; // the play animation's time at the last frame (its sounds)
	float play_yaw0 = 0;
	uint32_t pad_prev = prev;
	SpriteCache sprites;
	uint64_t frame_no = 0;
	float spin = 0; // L2 / R2: the middle game's turn
	float circ = 0; // 0..1: the middle cartridge turned to show its connector (the microphone on)
	bool circ_on = false; // a swipe up on the touchpad turns it, a swipe down back (user: the mic button is the PS5's mute)
	int swipe_y0 = -1; // where the touch began (touchpad y), and whether it made a swipe already
	bool swipe_done = false;
	float shutter = 0, shutter_grab = 0; // the middle floppy's shutter (0 closed .. 1 open), where the swipe began
	int touch_x0 = -1;
	float clam_open = 0, clam_goal = 0; // 2026-10-16: the middle clamshell box opened by a swipe left (right: shut again)
	float ins_open = 0, ins_goal = 0;   // 2026-10-16: the middle normal box's insert slid out by a swipe down (up: back in)
	int ins_x0 = -1;
	bool ins_done = false;
	int clam_x0 = -1, clam_sel = -1;
	bool clam_done = false;
	double style_since = -10; // L3: when the shelf's style changed (its name under the title)
	double pulse_until = 0; // a haptic pulse: until when, how strong (large, small motor)
	float pulse_large = 0, pulse_small = 0;
	int pulse_amp = 2, pulse_freq = 30; // the pulse on the triggers (2: the user found 5 too strong)
	auto pulse = [&](double secs, float large, float small, int amp = 2, int freq = 30) {
		pulse_until = Now() + std::max(secs, 0.1);
		pulse_large = large, pulse_small = small;
		pulse_amp = amp, pulse_freq = freq;
	};
	float circ_goal = 0; // where the connector view is heading (0 or 1)
	std::vector<Dust> dust;
	uint32_t dust_seed = 12345;
	double fav_since = -10; // Circle: when a favourite was added / removed (the note under the title)
	bool fav_added = false;
	float fav_pulse = 0; // its pulse in the controller
	int spin_notch = 0;
	int trigger_state = -1; // what the triggers were last set to (0 free, 1 resistance, else a buzz)
	for (;;)
	{
		GENPLUS_STAGE(Shelf, "loop");
		const double now = Now();
		const float dt = float(std::min(0.05, now - last));
		last = now;

		// MSX PS5 1.2: a floppy put in (or taken out) while the shelf is up: the shelf again, which starts its game
		{
			static double s_flop_check = 0;
			static std::string s_flop_sig;
			static bool s_flop_known = false;
			if (play_t0 < 0 && now - s_flop_check > 2.0)
			{
				s_flop_check = now;
				char devs[FE_FLOPPY_MAX][32];
				long long sizes[FE_FLOPPY_MAX];
				const int nf = fe_floppy_list(devs, sizes, FE_FLOPPY_MAX);
				std::string sig;
				for (int i = 0; i < nf; i++)
					sig += std::string(devs[i]) + ":" + std::to_string(sizes[i]) + "\n";
				if (!s_flop_known || frames_since_open == 0)
					s_flop_sig = sig, s_flop_known = true;
				else if (sig != s_flop_sig)
				{
					s_flop_sig = sig;
					OrbisLog("[shelf] the floppy drives changed (%d disk(s) now): looking for games again", nf);
					return kShelfRescan;
				}
			}
			frames_since_open++;
		}

		// -- input
		sound::Pump();
		ps5input::Poll();
		const uint32_t pad = ps5input::Pad(0).buttons;
		const uint32_t cur = play_t0 >= 0 ? 0 : pad; // no browsing while the game goes in
		const uint32_t down = cur & ~prev;
		prev = cur;
		const int old_sel = sel;
		if (n > 0)
		{
			if (rep_l.Fire(cur & SCE_PAD_BUTTON_LEFT, now))
				sel = std::max(0, sel - 1);
			if (rep_r.Fire(cur & SCE_PAD_BUTTON_RIGHT, now))
				sel = std::min(n - 1, sel + 1);
			if (rep_l1.Fire(cur & SCE_PAD_BUTTON_L1, now))
				sel = std::max(0, sel - 10);
			if (rep_r1.Fire(cur & SCE_PAD_BUTTON_R1, now))
				sel = std::min(n - 1, sel + 10);
		}
		if (sel != old_sel)
			sound::Play(sound::Sfx::Browse);
		if ((down & (SCE_PAD_BUTTON_UP | SCE_PAD_BUTTON_DOWN)) && tabs.size() > 1)
		{
			// another system: the shelf shows its games, the selection stays on the same game if it is there
			const std::string keep = n > 0 ? st.games[size_t(sel)].path : cfg.last_rom;
			const int dir = (down & SCE_PAD_BUTTON_DOWN) ? 1 : -1;
			tab = (tab + dir + int(tabs.size())) % int(tabs.size());
			st.family = int(tabs[size_t(tab)]);
			cfg.shelf_family = st.family;
			st.games = InFamily(st.all, tabs[size_t(tab)], cfg.one_version);
			st.slots.assign(st.games.size(), nullptr);
			st.covers.Start(st.games, cfg.covers_download && ShelfDownloads(), cfg.covers_download && BackgroundCovers());
			n = int(st.games.size());
			sel = 0;
			for (int i = 0; i < n; i++)
				if (st.games[size_t(i)].path == keep)
					sel = i;
			pos = float(sel);
			st.covers.SetFocus(sel);
			sel_since = now;
			tab_since = now;
			OrbisLog("[shelf] tab %s: %d game(s)", TabName(tabs[size_t(tab)]), n);
			dirty = true;
		}
		if (sel != old_sel)
		{
			st.covers.SetFocus(sel);
			sel_since = now;
			dirty = true;
		}
		if ((down & SCE_PAD_BUTTON_CROSS) && n > 0 && play_t0 < 0)
		{
			cfg.last_rom = st.games[size_t(sel)].path;
			cfg.Save();
			if (st.slots[size_t(sel)] && st.slots[size_t(sel)]->art)
			{
				play_t0 = now; // the game goes in first
				play_pressed = now;
#if defined(MSX_ANDROID)
				msx_android_shelf_snapshot(); // (the bottom screen keeps the shelf as it is now while the game plays)
#endif
				sound::Music(false);
				play_yaw0 = 0.16f * float(std::sin(now * 1.15));
				if (clam_open > 0.5f || ins_open > 0.5f)
					play_t0 = now - 1.8, play_yaw0 = 0; // (open already / insert out: from there on)
				pos = float(sel);
			}
			else
				return st.games[size_t(sel)].path;
		}
		if (play_t0 >= 0 && play_t0 < now)
		{
			const bool skip = now > play_pressed && (pad & ~pad_prev & (SCE_PAD_BUTTON_CROSS | SCE_PAD_BUTTON_CIRCLE)) != 0;
			const bool bx = ShelfStyle() == 0;
			const cart3d::Kind pm = !st.slots[size_t(sel)] ? cart3d::Kind::Cartridge
				: bx ? st.slots[size_t(sel)]->media : ItemKind(*st.slots[size_t(sel)], ShelfStyle());
			const bool clam = st.slots[size_t(sel)] && st.slots[size_t(sel)]->art && st.slots[size_t(sel)]->art->clamshell;
			if (skip || now - play_t0 > PlayTime(bx, pm, clam, st.slots[size_t(sel)] ? ItemKind(*st.slots[size_t(sel)], 0) : cart3d::Kind::BoxWide,
						!IsArabicEdition(st.games[size_t(sel)])))
				return st.games[size_t(sel)].path;
		}
		pad_prev = pad;
		if (down & SCE_PAD_BUTTON_TRIANGLE)
		{
			sound::Play(sound::Sfx::Open);
			SettingsMenu();
			sound::Play(sound::Sfx::Close);
			sprites.Clear(); // (the shelf's style may have changed)
			if (st.one_version != cfg.one_version)
			{
				// one version of each game, or all: the same game stays selected if it is there
				const std::string keep = n > 0 ? st.games[size_t(sel)].path : cfg.last_rom;
				st.one_version = cfg.one_version;
				st.games = InFamily(st.all, tabs[size_t(tab)], cfg.one_version);
				st.slots.assign(st.games.size(), nullptr);
				st.covers.Start(st.games, cfg.covers_download && ShelfDownloads(), cfg.covers_download && BackgroundCovers());
				n = int(st.games.size());
				sel = 0;
				for (int i = 0; i < n; i++)
					if (st.games[size_t(i)].path == keep)
						sel = i;
				pos = float(sel);
				st.covers.SetFocus(sel);
				sel_since = now;
			}
			if (st.downloads != cfg.covers_download)
			{
				st.downloads = cfg.covers_download;
				st.covers.Start(st.games, cfg.covers_download && ShelfDownloads(), cfg.covers_download && BackgroundCovers());
				st.slots.assign(st.games.size(), nullptr);
			}
			ps5input::Poll();
			prev = ps5input::Pad(0).buttons;
			dirty = true;
		}
		if ((down & SCE_PAD_BUTTON_CIRCLE) && n > 0)
		{
			const bool fav = ToggleFavorite(st.games[size_t(sel)]);
			sound::Play(sound::Sfx::Favorite);
			fav_since = now, fav_added = fav, fav_pulse = 1.0f;
			pulse(0.14, 255, 255, 6, 40);
			const bool has_tab = std::find(tabs.begin(), tabs.end(), kFavoritesTab) != tabs.end();
			if (fav && !has_tab)
			{
				tabs.insert(tabs.begin() + 1, kFavoritesTab); // the Favorites tab comes with the first one
				if (tab >= 1)
					tab++;
			}
			else if (!fav && tabs[size_t(tab)] == kFavoritesTab)
			{
				// off the Favorites tab's list
				st.games = InFamily(st.all, kFavoritesTab, cfg.one_version);
				st.slots.assign(st.games.size(), nullptr);
				st.covers.Start(st.games, cfg.covers_download && ShelfDownloads(), cfg.covers_download && BackgroundCovers());
				n = int(st.games.size());
				sel = std::clamp(sel, 0, std::max(0, n - 1));
				pos = float(sel);
				st.covers.SetFocus(sel);
				sel_since = now;
			}
			dirty = true;
		}
		if (down & SCE_PAD_BUTTON_L3)
		{
			// the shelf's style: boxes, cartridges, floppies
			cfg.shelf_style = (cfg.shelf_style + 1) % 3;
			cfg.Save();
			sprites.Clear();
			style_since = now;
			pulse(0.10, 255, 255, 6, 40);
			dirty = true;
		}
#if defined(MSX_ANDROID)
		if (false) // (the Thor: R3 is easy to click by accident - no cover turning there)
#else
		if ((down & SCE_PAD_BUTTON_R3) && n > 0 && ShelfStyle() == 0)
#endif
		{
			// a cover stored sideways: a quarter turn (remembered)
			TurnCover(st.games[size_t(sel)]);
			st.covers.Reload(sel);
			st.slots[size_t(sel)] = nullptr;
			bd_wanted = nullptr;
			dirty = true;
		}
		if ((down & SCE_PAD_BUTTON_SQUARE) && n > 0 && tabs[size_t(tab)] == kDriveTab)
		{
			// MSX PS5 1.2: the floppy's game into the collection
			bool changed = false;
			const std::string said = AddFloppyToCollection(st.games[size_t(sel)], st.all, &changed);
			if (!said.empty())
			{
				Loading(said.c_str());
				sound::Play(changed ? sound::Sfx::Favorite : sound::Sfx::Close);
				usleep(1300000);
			}
			if (changed)
				return kShelfRescan; // (the shelf again: the game is in its tab now too)
			ps5input::Poll();
			prev = ps5input::Pad(0).buttons;
			dirty = true;
		}
		else if ((down & SCE_PAD_BUTTON_SQUARE) && n > 0)
		{
			st.covers.Refetch(sel);
			st.slots[size_t(sel)] = nullptr;
			dirty = true;
			if (!ShelfDownloads()) // the console: the helper fetches it (or the prefetch at the next start)
			{
				cfg.last_rom = st.games[size_t(sel)].path;
				cfg.Save();
				CoversRestartIfNeeded(st.all, cfg.covers_download, true, Loading);
			}
		}
		if (down & SCE_PAD_BUTTON_OPTIONS)
		{
			if (Confirm("Quit MSX PS5?"))
				return "";
			ps5input::Poll();
			prev = ps5input::Pad(0).buttons;
			dirty = true;
		}

		// -- covers that arrived
		const size_t before = std::count_if(st.slots.begin(), st.slots.end(), [](const CoverPtr& c) { return c != nullptr; });
		st.covers.Collect(st.slots, kKeepRadius);
		const size_t after = std::count_if(st.slots.begin(), st.slots.end(), [](const CoverPtr& c) { return c != nullptr; });
		if (after != before)
			dirty = true;

		// -- glide
		const float target = float(sel);
		if (std::abs(target - pos) > 0.0005f)
		{
			pos += (target - pos) * (1.0f - std::exp(-dt * 11.0f));
			if (std::abs(target - pos) < 0.0005f)
				pos = target;
			dirty = true;
		}

		// -- backdrop follows the selection once it rests for a moment
		const CoverTex* sel_tex = (n > 0 && st.slots[size_t(sel)]) ? st.slots[size_t(sel)].get() : nullptr;
		if (cfg.shelf_theme != bd_theme)
		{
			// the theme changed (Settings): the backdrop again, at once
			bd_theme = cfg.shelf_theme;
			bd_old.px.clear();
			bd_wanted = nullptr;
			if (bd_theme > 0)
				BuildThemeBackdrop(pool, bd_cur, bd_theme);
			else
				BuildBackdrop(pool, bd_cur, sel_tex);
			fade = 1.0f;
			dirty = true;
#if defined(MSX_ANDROID)
			msx_android_set_theme(bd_theme);
			if (bd_theme == 0)
				SendTopPicture(pool, sel_tex, n > 0 ? &st.games[size_t(sel)] : nullptr);
#endif
		}
		if (bd_theme == 0 && sel_tex && sel_tex != bd_cur.from && sel_tex != bd_wanted && now - sel_since > 0.12)
		{
			bd_wanted = sel_tex;
			std::swap(bd_old, bd_cur);
			BuildBackdrop(pool, bd_cur, sel_tex);
			fade = bd_old.px.empty() ? 1.0f : 0.0f;
#if defined(MSX_ANDROID)
			SendTopPicture(pool, sel_tex, &st.games[size_t(sel)]); // (Cover glow: the top screen glows with the cover too)
#endif
			dirty = true;
		}
		if (fade < 1.0f)
		{
			fade = std::min(1.0f, fade + dt * 4.0f);
			dirty = true;
		}

		const int status = (st.covers.ToDownload() + st.covers.BackgroundLeft()) * 1000 + st.covers.Downloaded() +
						   (st.covers.BackgroundOffline() ? 1 << 30 : 0);
		if (status != last_status)
		{
			last_status = status;
			dirty = true;
		}

		// the middle game sways (always moving: drawn every frame); the play animation's frame
		const float sway = 0.16f * float(std::sin(now * 1.15));
		dirty = true;
		const bool playing = play_t0 >= 0;
		const bool boxes = ShelfStyle() == 0;
		PlayShot shot;
		if (playing && n > 0 && st.slots[size_t(sel)])
			shot = PlayAt(float(now - play_t0), boxes, ItemKind(*st.slots[size_t(sel)], 0),
				boxes ? st.slots[size_t(sel)]->media : ItemKind(*st.slots[size_t(sel)], ShelfStyle()), play_yaw0, false,
				st.slots[size_t(sel)]->art && st.slots[size_t(sel)]->art->clamshell, !IsArabicEdition(st.games[size_t(sel)]));
		// the play animation's sounds: the cartridge's clack in the slot; the floppy's slide and latch, then the seek
		if (playing && n > 0 && st.slots[size_t(sel)])
		{
			const cart3d::Kind pm = boxes ? st.slots[size_t(sel)]->media : ItemKind(*st.slots[size_t(sel)], ShelfStyle());
			const float t = float(now - play_t0);
			auto at = [&](float when) { return play_sfx_t < when && t >= when; };
			if (pm == cart3d::Kind::Floppy)
			{
				const float fa = kFloppyAt[boxes ? 1 : 0];
				if (at(fa + 0.35f))
					sound::Play(sound::Sfx::FloppyInsert); // (its latch clicks as the disk is all in)
				if (at(fa + 1.05f))
					sound::Play(sound::Sfx::FloppySeek, 0.8f);
			}
			else if (shot.clam || shot.ins)
			{
				if (at(0.8f) && t < 1.2f)
					sound::Play(sound::Sfx::Open); // the lid unlatching
				if (at(4.08f))
					sound::Play(sound::Sfx::CartClack);
			}
			else if (at(boxes ? 3.38f : 2.62f))
				sound::Play(sound::Sfx::CartClack);
			play_sfx_t = t;
		}
		else
			play_sfx_t = -1;

		// -- the microphone on: the middle cartridge turns to show its connector; blowing into it raises dust
		float blow = 0, mid_yaw = 0;
		bool circ_from_box = true;
		{
			const CoverTex* mid = n > 0 ? st.slots[size_t(sel)].get() : nullptr;
			const bool can = mid && mid->art && mid->media == cart3d::Kind::Cartridge && ShelfStyle() == 1 && !playing &&
				std::abs(float(sel) - pos) < 0.02f;
			// the touchpad on a cartridge: swipe up = turn it to show its connector, swipe down = back (user request; the mic
			// button stays the PS5's mute, which an app can't set: sceAudioInSetUserMute 8026010f); a touch that stays put blows
			const ps5input::PadState& tp = ps5input::Pad(0);
			int touch_dy = 0;
			if (tp.fingers > 0)
			{
				if (swipe_y0 < 0)
					swipe_y0 = tp.ty, swipe_done = false;
				touch_dy = int(tp.ty) - swipe_y0;
				const bool cart_ok = mid && mid->art && mid->media == cart3d::Kind::Cartridge && ShelfStyle() == 1 && !playing;
				if (!swipe_done && cart_ok && touch_dy < -200)
					circ_on = true, swipe_done = true;
				else if (!swipe_done && touch_dy > 200)
					circ_on = false, swipe_done = true;
			}
			else
				swipe_y0 = -1;
			if (!can)
				circ_on = circ_on && n > 0 && !playing; // (kept while browsing: it turns again on the next cartridge)
			const float target = (can && circ_on) ? 1.0f : 0.0f; // (not on sound alone: user, it came up in the circuit view)
			circ_goal = target;
			circ += (target - circ) * (1.0f - std::exp(-dt * 4.0f));
			if (std::abs(circ - target) < 0.002f)
				circ = target;
			circ_from_box = !(mid && ItemKind(*mid, ShelfStyle()) == cart3d::Kind::Cartridge);
#if defined(MSX_ANDROID)
			msx_android_touch_hold(circ > 0.3f); // (cart raised: a finger held still blows, a tap doesn't play)
#endif
			blow = circ > 0.85f ? std::max(ps5mic::Blow(), (tp.fingers > 0 && !swipe_done && std::abs(touch_dy) < 80) ? 1.0f : 0.0f)
							   : 0.0f;
			mid_yaw = sway * std::max(0.0f, 1 - std::abs(spin) * 2) + spin;
			auto rnd = [&] {
				dust_seed = dust_seed * 1664525u + 1013904223u;
				return float(dust_seed >> 8) / 16777216.0f;
			};
			if (blow > 0.05f)
			{
				// out of the connector: its middle and half width on screen
				const cart3d::Pose cp = CircuitPose(circ, mid_yaw, circ_from_box);
				const float sx = W * 0.5f + kFocal * cp.x / cp.z, sy = kHorizon - kFocal * (cp.lift - kCamY) / cp.z;
				const float hw = kFocal * 0.42f * cp.height / cp.z;
				// a few puffs a second, out of the opening, drifting down every which way
				const int count = int(blow * 150 * dt + rnd());
				for (int k = 0; k < count && dust.size() < 150; k++)
				{
					Dust d;
					d.x = sx + (rnd() * 2 - 1) * hw * 0.8f, d.y = sy + rnd() * 10;
					const float ang = 1.5708f + (rnd() * 2 - 1) * 1.1f; // around straight down, wide
					const float speed = 260 + rnd() * 420;
					d.vx = std::cos(ang) * speed + (d.x - sx) * 0.25f, d.vy = std::sin(ang) * speed;
					d.age = 0, d.life = 1.4f + rnd() * 1.2f, d.size = 22 + rnd() * 22;
					const int g = 150 + int(rnd() * 50);
					d.color = Rgb(uint8_t(g), uint8_t(g - 6), uint8_t(g - 18));
					d.nu = rnd() * 128, d.nv = rnd() * 128;
					dust.push_back(d);
				}
			}
			for (Dust& d : dust)
			{
				d.age += dt;
				d.vx *= std::exp(-dt * 0.55f), d.vy *= std::exp(-dt * 0.4f); // the air slows it (a little)
				d.vy += 14 * dt;                                              // settling
				d.vx += std::sin(d.age * 1.7f + d.nu) * 30 * dt;              // and swirling
				d.x += d.vx * dt, d.y += d.vy * dt;
				d.size = std::min(170.0f, d.size + 95 * dt); // spreading (to a point: the frame's time)
				d.nv += 6 * dt;    // its wisps rolling
			}
			dust.erase(std::remove_if(dust.begin(), dust.end(), [](const Dust& d) { return d.age >= d.life || d.y > H + 20; }),
				dust.end());
		}

		// -- the touchpad on a clamshell box (the Sakhr collection): swipe left = open it, swipe right = shut it
		{
			const ps5input::PadState& tp = ps5input::Pad(0);
			const CoverTex* mid = n > 0 ? st.slots[size_t(sel)].get() : nullptr;
			// (not while it is being turned: user, the halves came apart)
			const bool still = std::abs(spin) < 0.01f;
			const bool can = mid && mid->art && mid->art->clamshell && ShelfStyle() == 0 && !playing && std::abs(float(sel) - pos) < 0.02f && still;
			if (sel != clam_sel)
				clam_sel = sel, clam_goal = 0, clam_open = 0, ins_goal = 0, ins_open = 0; // (browsing: the next box comes shut)
			if (tp.fingers > 0)
			{
				if (clam_x0 < 0)
					clam_x0 = tp.tx, clam_done = false;
				const int dx = int(tp.tx) - clam_x0;
				if (!clam_done && can && dx < -250)
				{
					clam_done = true;
					if (clam_goal < 0.5f)
						clam_goal = 1, sound::Play(sound::Sfx::Open);
				}
				else if (!clam_done && can && dx > 250)
				{
					clam_done = true;
					if (clam_goal > 0.5f)
						clam_goal = 0, sound::Play(sound::Sfx::Close);
				}
			}
			else
				clam_x0 = -1;
			if (!can && !playing && still)
				clam_goal = 0;
			// a normal box: swipe right = its insert slides out sideways (the cartridge in its tray), swipe left = back in
			const bool can_ins = mid && mid->art && !mid->art->clamshell && !IsArabicEdition(st.games[size_t(sel)]) && ShelfStyle() == 0 && !playing && still &&
				std::abs(float(sel) - pos) < 0.02f && mid->media == cart3d::Kind::Cartridge && ItemKind(*mid, 0) == cart3d::Kind::Box;
			if (tp.fingers > 0)
			{
				if (ins_x0 < 0)
					ins_x0 = tp.tx, ins_done = false;
				const int dx = int(tp.tx) - ins_x0;
				if (!ins_done && can_ins && dx > 250)
				{
					ins_done = true;
					if (ins_goal < 0.5f)
						ins_goal = 1, sound::Play(sound::Sfx::Open);
				}
				else if (!ins_done && can_ins && dx < -250)
				{
					ins_done = true;
					if (ins_goal > 0.5f)
						ins_goal = 0, sound::Play(sound::Sfx::Close);
				}
			}
			else
				ins_x0 = -1;
			if (!can_ins && !playing && still)
				ins_goal = 0;
			ins_open += (ins_goal - ins_open) * (1.0f - std::exp(-dt * 5.0f));
			if (std::abs(ins_open - ins_goal) < 0.002f)
				ins_open = ins_goal;
			clam_open += (clam_goal - clam_open) * (1.0f - std::exp(-dt * 5.0f));
			if (std::abs(clam_open - clam_goal) < 0.002f)
				clam_open = clam_goal;
		}

		// -- the touchpad on a floppy: a swipe slides its shutter open; let go, it springs back
		const float shutter_prev = shutter;
		{
			const ps5input::PadState& tp = ps5input::Pad(0);
			const CoverTex* mid = n > 0 ? st.slots[size_t(sel)].get() : nullptr;
			const bool floppy = mid && mid->art && ItemKind(*mid, ShelfStyle()) == cart3d::Kind::Floppy && !playing &&
				std::abs(float(sel) - pos) < 0.02f;
			if (floppy && tp.fingers > 0)
			{
				if (touch_x0 < 0)
					touch_x0 = tp.tx, shutter_grab = shutter;
				shutter = std::clamp(shutter_grab + std::abs(float(int(tp.tx) - touch_x0)) / 600.0f, 0.0f, 1.0f);
			}
			else
			{
				touch_x0 = -1;
				shutter += (0.0f - shutter) * (1.0f - std::exp(-dt * 14.0f));
				if (shutter < 0.002f)
					shutter = 0;
			}
			if (!floppy)
				shutter = 0;
			if (shutter_prev < 0.06f && shutter >= 0.06f && tp.fingers > 0)
				sound::Play(sound::Sfx::ShutterSlide); // (user: the slide had no sound)
			if (shutter_prev < 0.98f && shutter >= 0.98f)
				pulse(0.10, 255, 255, 3, 40); // all the way open
			if (shutter_prev > 0.08f && shutter <= 0.08f && tp.fingers == 0)
			{
				pulse(0.10, 255, 220, 3, 40); // snapped shut
				sound::Play(sound::Sfx::ShutterSnap);
			}
		}

		// -- L2 / R2 turn the middle game round (R2 to the right, L2 to the left; all the way down: its back), with a
		//    click in the controller every eighth of a turn; the triggers stiffen as they go down (TriggerSlope).
		//    A thump when the game goes into the slot.
		{
			const ps5input::PadState& pad1 = ps5input::Pad(0);
			const bool opened = clam_open > 0.001f || clam_goal > 0 || ins_open > 0.001f || ins_goal > 0;
			const float target = (playing || opened) ? 0.0f : (float(pad1.r2) - float(pad1.l2)) / 255.0f * 3.14159265f;
			spin += (target - spin) * (1.0f - std::exp(-dt * 12.0f));
			if (std::abs(spin) < 0.0005f && target == 0)
				spin = 0;
			const int notch = int(std::lround(spin / (3.14159265f / 8)));
			if (notch != spin_notch)
			{
				spin_notch = notch;
				pulse(std::abs(notch) == 8 ? 0.16 : 0.10, 255, 255); // (the turning buzz: amplitude 5, 30 Hz)
			}
			const float thump = std::max(playing ? shot.flash : 0.0f, fav_pulse);
			fav_pulse *= std::exp(-dt * 14.0f);
			// turning to show its connector: a hum; blowing: a rush of air, grainy
			// turning: only while the turn is under way (the ease's long tail doesn't count), or the shutter sliding
			const float turning = (std::abs(circ - circ_goal) > 0.04f || std::abs(shutter - shutter_prev) > 0.002f) ? 1.0f : 0.0f;
			const float grain = 0.6f + 0.4f * float(std::sin(now * 97.0) * std::sin(now * 61.0));
			const bool pulsing = now < pulse_until;
			ps5input::Rumble(uint8_t(std::min(255.0f, std::max(pulsing ? pulse_large : 0.0f, 255 * thump + 255 * blow * grain + 120 * turning))),
				uint8_t(std::min(255.0f, std::max(pulsing ? pulse_small : 0.0f, 200 * thump + 255 * turning + 230 * blow * (1.4f - grain)))));
			// the triggers: a buzz for the haptics above (their motors: the rumble isn't felt in this app), else
			// their resistance while the shelf is up, free while a game goes in
			int amp = 0, freq = 30;
			if (pulsing)
				amp = pulse_amp, freq = pulse_freq;
			else if (turning > 0)
				amp = 2, freq = 30; // turning (the user's 30 Hz; strength 5 was too strong)
			else if (blow > 0.05f)
				amp = 2, freq = 50; // blowing (the user's setting)
			else if (thump > 0.2f)
				amp = 8, freq = 25;
			const int want = playing && amp == 0 ? 0 : amp > 0 ? 100 + amp * 1000 + freq : 1; // (what the triggers do now)
			if (want != trigger_state)
			{
				trigger_state = want;
				if (want == 0)
					ps5input::TriggersOff();
				else if (want == 1)
					ps5input::TriggerSlope(1, 9, 1, 6);
				else
					ps5input::TriggerVibration(0, uint8_t(amp), uint8_t(freq));
			}
		}
		if (!dirty)
		{
			// nothing moved: flip a 1-pixel change so the loop still runs at the display's pace
			ps5video::Present(0, 0, 1, 1, true);
			continue;
		}
		dirty = false;

		// -- place the covers, far ones first
		std::vector<Placed> placed;
		if (n > 0)
		{
			const int lo = std::max(0, int(std::floor(pos - kVisible)));
			const int hi = std::min(n - 1, int(std::ceil(pos + kVisible)));
			for (int i = lo; i <= hi; i++)
			{
				const CoverTex* tex = st.slots[size_t(i)] ? st.slots[size_t(i)].get() : LoadingCard().get();
				Placed p;
				if (Place(p, tex, float(i) - pos))
				{
					p.hold = st.slots[size_t(i)];
					placed.push_back(p);
				}
			}
			std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return std::abs(a.d) > std::abs(b.d); });
		}
		// -- the side games' sprites: the ones on screen first, then the selection and its neighbours on both sides
		const double t_render = Now();
		const cart3d::View view{W, H, kFocal, kHorizon, kCamY};
		frame_no++;
		const bool moving = std::abs(float(sel) - pos) > 0.002f;
		for (const Placed& p : placed)
			if (p.hold && p.hold->art && std::abs(p.d) >= kSideFrom)
				sprites.Want(p.hold, p.d < 0 ? -1 : 1, ItemKind(*p.hold, ShelfStyle()), frame_no);
		for (int i = std::max(0, sel - 2); n > 0 && i <= std::min(n - 1, sel + 2); i++)
		{
			const CoverPtr& t = st.slots[size_t(i)];
			if (t && t->art)
				for (int side : {-1, 1})
					sprites.Want(t, side, ItemKind(*t, ShelfStyle()), frame_no);
		}
		const int made = sprites.Make(pool, view, 0.006, frame_no);
		std::vector<const cart3d::Sprite*> spr(placed.size(), nullptr);
		for (size_t i = 0; i < placed.size(); i++)
		{
			const Placed& p = placed[i];
			if (p.hold && p.hold->art && std::abs(p.d) >= kSideFrom)
				spr[i] = sprites.Find(p.hold.get(), p.d < 0 ? -1 : 1, ItemKind(*p.hold, ShelfStyle()), frame_no);
		}
		uint32_t* surf = ps5video::Surface();
		const uint32_t fade_w = uint32_t(fade * 256);
		pool.Run([&](int x0, int x1) {
			// backdrop
			for (int y = 0; y < H; y++)
			{
				uint32_t* out = surf + size_t(y) * W;
				const uint32_t* a = bd_cur.px.data() + size_t(y) * W;
				if (fade_w >= 256 || bd_old.px.empty())
					memcpy(out + x0, a + x0, size_t(x1 - x0) * 4);
				else
				{
					const uint32_t* o = bd_old.px.data() + size_t(y) * W;
					for (int x = x0; x < x1; x++)
						out[x] = Lerp32(o[x], a[x], fade_w);
				}
			}
			// the games, far ones first: sprites at the sides, 3D in the middle (the flat cover until a game's art is made)
			const float dimk = 1 - shot.dim;
			for (size_t i = 0; i < placed.size(); i++)
			{
				const Placed& p = placed[i];
				const float ad = std::abs(p.d);
				const bool selected = ad < 0.5f;
				if (!p.tex->art)
				{
					DrawCover(p, surf, x0, x1);
					continue;
				}
				const cart3d::Art& art = *p.tex->art;
				const cart3d::Kind kind = ItemKind(*p.tex, ShelfStyle());
				if (spr[i])
				{
					// a side game: its sprite, moved along the row and dimmer the further it is
					const int side = p.d < 0 ? -1 : 1;
					const int dx = int(std::lround(kFocal * side * std::max(0.0f, ad - 1) * kSideStep / (kCamDist + kSideZ)));
					const float b = p.bright / 256.0f;
					cart3d::Blit(surf, view, *spr[i], dx, std::sqrt(b / kSideBright) * dimk, p.alpha / 256.0f, x0, x1);
					continue;
				}
				if (playing && selected)
				{
					// the play animation: the box, the slot in the floor, the cartridge / floppy
					if (shot.clam)
					{
						// the clamshell opening: its two halves (each with its inside), the cartridge in the lid's tray
						cart3d::Art back_art = art, lid_art = art;
						back_art.half = 2;
						lid_art.half = 1;
						lid_art.back = art.front; // (the lid's outside is the cover)
						cart3d::Draw(surf, view, cart3d::Kind::BoxHalf, shot.back_half, back_art, x0, x1);
						if (!shot.cart_over_lid)
							cart3d::Draw(surf, view, cart3d::Kind::Cartridge, shot.item, art, x0, x1);
						cart3d::Draw(surf, view, cart3d::Kind::BoxHalf, shot.lid, lid_art, x0, x1);
						DrawSlot(surf, x0, x1, shot);
						if (shot.cart_over_lid)
							cart3d::Draw(surf, view, cart3d::Kind::Cartridge, shot.item, art, x0, x1);
						continue;
					}
					if (shot.ins)
					{
						// a normal box's insert sliding out: the insert and the cartridge in it, the box over them
						cart3d::Art ins_art = art;
						ins_art.half = 3, ins_art.clamshell = true; // (a moulded tray, bare plastic round its edges)
						if (shot.box)
							cart3d::Draw(surf, view, cart3d::Kind::BoxHalf, shot.insert, ins_art, x0, x1);
						if (!shot.cart_out)
							cart3d::Draw(surf, view, cart3d::Kind::Cartridge, shot.item, art, x0, x1);
						if (shot.box)
							cart3d::Draw(surf, view, cart3d::Kind::Box, shot.box_pose, art, x0, x1);
						DrawSlot(surf, x0, x1, shot);
						if (shot.cart_out)
							cart3d::Draw(surf, view, cart3d::Kind::Cartridge, shot.item, art, x0, x1);
						continue;
					}
					if (shot.box)
						cart3d::Draw(surf, view, shot.box_kind, shot.box_pose, art, x0, x1);
					DrawSlot(surf, x0, x1, shot);
					DrawDrive(surf, x0, x1, shot, false);
					cart3d::Draw(surf, view, boxes ? p.tex->media : kind, shot.item, art, x0, x1);
					if (shot.drive_over)
						DrawDrive(surf, x0, x1, shot, true);
					continue;
				}
				cart3d::Pose q;
				q.x = p.cx, q.z = p.dist, q.yaw = p.yaw, q.height = ItemHeight(kind);
				if (ad < 1)
				{
					// the middle one turns gently, or as far as L2 / R2 turn it
					const float sw = sway * std::max(0.0f, 1 - std::abs(spin) * 2) * (1 - std::max(clam_open, ins_open));
					q.yaw += (sw + spin) * (1 - ad);
				}
				q.bright = p.bright / 256.0f * dimk;
				q.alpha = p.alpha / 256.0f;
				q.fast = moving;
				if (selected && kind == cart3d::Kind::Floppy)
					q.shutter = shutter;
				if (selected && !art.clamshell && ins_open > 0.001f && kind == cart3d::Kind::Box)
				{
					// its insert slid out by a swipe down: the insert on the floor with the cartridge in its tray, the box over it
					cart3d::Pose box_q = q, ins_q = q, cart_q = q;
					InsertPoses(ins_open, q.x, q.z + 200 * ins_open, q.yaw, box_q, ins_q, cart_q);
					cart_q.shadow = ins_q.shadow = false;
					cart3d::Art ins_art = art;
					ins_art.half = 3, ins_art.clamshell = true;
					cart3d::Draw(surf, view, cart3d::Kind::BoxHalf, ins_q, ins_art, x0, x1);
					cart3d::Draw(surf, view, cart3d::Kind::Cartridge, cart_q, art, x0, x1);
					cart3d::Draw(surf, view, cart3d::Kind::Box, box_q, art, x0, x1);
					continue;
				}
				if (selected && art.clamshell && clam_open > 0.001f && (kind == cart3d::Kind::Box || kind == cart3d::Kind::BoxWide))
				{
					// opened by a swipe: its two halves, the cartridge in the lid's tray, the manual in the back half
					cart3d::Pose back_q = q, lid_q = q, cart_q = q;
					bool over = false;
					ClamPoses(clam_open, q.x, q.z + 410 * clam_open, q.yaw, back_q, lid_q, cart_q, over);
					cart_q.shadow = back_q.shadow = lid_q.shadow = false;
					cart3d::Art back_art = art, lid_art = art;
					back_art.half = 2;
					lid_art.half = 1;
					lid_art.back = art.front; // (the lid's outside is the cover)
					cart3d::Draw(surf, view, cart3d::Kind::BoxHalf, back_q, back_art, x0, x1);
					if (!over)
						cart3d::Draw(surf, view, cart3d::Kind::Cartridge, cart_q, art, x0, x1);
					cart3d::Draw(surf, view, cart3d::Kind::BoxHalf, lid_q, lid_art, x0, x1);
					if (over)
						cart3d::Draw(surf, view, cart3d::Kind::Cartridge, cart_q, art, x0, x1);
					continue;
				}
				if (selected && circ > 0.001f)
				{
					// the microphone on: the cartridge turned to show its connector (out of its box / floppy)
					const cart3d::Pose c = CircuitPose(circ, mid_yaw, circ_from_box);
					if (!circ_from_box)
					{
						cart3d::Draw(surf, view, cart3d::Kind::Cartridge, c, art, x0, x1);
						continue;
					}
					q.bright *= 1 - 0.5f * circ;
					cart3d::Draw(surf, view, kind, q, art, x0, x1);
					cart3d::Draw(surf, view, cart3d::Kind::Cartridge, c, art, x0, x1);
					continue;
				}
				cart3d::Draw(surf, view, kind, q, art, x0, x1);
			}
			// the dust: smoke puffs - a soft falloff, broken into wisps by the noise, fading in fast and out slowly
			const uint8_t* noise = SmokeNoise();
			for (const Dust& d : dust)
			{
				const float t = d.age / d.life;
				const float a = std::min(1.0f, t * 8.0f) * (1 - t) * (1 - t) * 0.42f;
				if (a <= 0.003f)
					continue;
				const int r = int(d.size) + 1;
				const int xa = std::max(x0, int(d.x) - r), xb = std::min(x1 - 1, int(d.x) + r);
				const int ya = std::max(0, int(d.y) - r), yb = std::min(H - 1, int(d.y) + r);
				const float inv = 1.0f / d.size, scale = 40.0f / d.size; // the noise's size on the puff
				for (int yy = ya; yy <= yb; yy++)
					for (int xx = xa; xx <= xb; xx++)
					{
						const float dx = (xx + 0.5f - d.x) * inv, dy = (yy + 0.5f - d.y) * inv;
						const float r2 = dx * dx + dy * dy;
						if (r2 >= 1)
							continue;
						const float fall = (1 - r2) * (1 - r2);
						const int nx = int(d.nu + (xx - d.x) * scale) & 127, ny = int(d.nv + (yy - d.y) * scale) & 127;
						const float wisp = noise[ny * 128 + nx] / 255.0f;
						const float w = a * fall * std::clamp(wisp * 1.6f - 0.25f, 0.0f, 1.0f);
						if (w > 0.002f)
							surf[size_t(yy) * W + xx] = Lerp32(surf[size_t(yy) * W + xx], d.color, uint32_t(w * 256));
					}
			}
			// the end of the play animation: to black
			if (shot.fade > 0)
			{
				const uint32_t keep = uint32_t((1 - shot.fade) * 256);
				for (int y = 0; y < H; y++)
				{
					uint32_t* row = surf + size_t(y) * W;
					for (int x = x0; x < x1; x++)
						row[x] = Scale32(row[x], keep);
				}
			}
		});

		{
			static double sum = 0;
			static int frames = 0;
			sum += Now() - t_render;
			if (++frames == 60)
			{
				OrbisLog("[shelf] 3D pass: %.2f ms a frame (average of 60; sprites cached %d)", sum / frames * 1000.0, made);
				sum = 0;
				frames = 0;
			}
		}
		// -- text (none while the game goes in)
		if (!playing)
		{
			DrawText(80, 46, "MSX PS5", 6, Rgb(170, 150, 255));
			// (the credits are in Settings > ABOUT: user request, the shelf kept clean)
			if (tabs.size() > 1)
			{
				const std::string label = std::string(icon::DpadUpDown) + "  " + TabName(tabs[size_t(tab)]);
				const bool flash = now - tab_since < 1.2;
				CenterText(flash ? 52 : 60, label, flash ? 5 : 4, flash ? Rgb(245, 240, 255) : Rgb(200, 190, 240));
				if (flash)
					dirty = true;
			}
			if (n > 0)
			{
				char count[64];
				snprintf(count, sizeof(count), "%d / %d", sel + 1, n);
				DrawText(W - 80 - TextWidth(count, 4), 50, count, 4, Rgb(220, 220, 235));
				const GameInfo& g = st.games[size_t(sel)];
				CenterText(800, g.title, 6, Rgb(245, 245, 252));
				if (IsFavorite(g))
				{
					const int tw = TextWidth(FitText(g.title, 6, W - 200).c_str(), 6);
					DrawStar((W - tw) / 2.0f - 44, 828, 24, Rgb(255, 200, 60));
				}
				if (now - style_since < 1.2)
					CenterText(752, ShelfStyle() == 2 ? "Floppies" : ShelfStyle() == 1 ? "Cartridges" : "Boxes", 4,
						Rgb(255, 220, 140));
				if (now - fav_since < 1.4)
				{
					CenterText(752, fav_added ? "Added to Favorites" : "Removed from Favorites", 3, Rgb(255, 210, 110));
					dirty = true;
				}
				std::string sub = std::string(Info(g.system).name) + "   ";
				if (!g.region.empty())
					sub += g.region + "   ";
				std::string ext = g.ext.size() > 1 ? g.ext.substr(1) : g.ext;
				for (char& c : ext)
					c = char(toupper(uint8_t(c)));
				if (strcasecmp(ext.c_str(), Info(g.system).name) != 0)
					sub += ext;
				if (g.on_usb)
					sub += "   USB";
				if (sel_tex && !sel_tex->real)
					sub += "   no cover";
				CenterText(880, sub, 3, Rgb(175, 170, 205));
			}
			else
			{
				CenterText(300, "No games found.", 5, Rgb(240, 240, 250));
				CenterText(400, "Copy your MSX games to", 3, Rgb(200, 200, 220));
				CenterText(450, "/data/msx/roms   or, on a USB drive,   msx/roms  or  roms/msx1, msx2...", 4, Rgb(200, 190, 255));
				CenterText(520, "Cartridges  .rom .mx1 .mx2     Disks  .dsk     Tapes  .cas     (also inside .zip)", 3,
					Rgb(185, 180, 210));
				CenterText(640, "The MSX BIOS: blueMSX's Machines and Databases folders in /data/msx/bios (or a drive's bios).", 3,
					Rgb(200, 200, 220));
			}
			// download status
			if (st.covers.ToDownload() > 0 && !st.covers.Offline() && cfg.covers_download)
			{
				char s[96];
				snprintf(s, sizeof(s), "Downloading covers... %d left", st.covers.ToDownload());
				DrawText(W - 80 - TextWidth(s, 3), 112, s, 3, Rgb(170, 160, 210));
			}
			else if (st.covers.BackgroundLeft() > 0 && !st.covers.BackgroundOffline() && cfg.covers_download)
			{
				char s[96];
				snprintf(s, sizeof(s), "Downloading covers in the background... %d left", st.covers.BackgroundLeft());
				DrawText(W - 80 - TextWidth(s, 3), 112, s, 3, Rgb(170, 160, 210));
			}
			else if ((st.covers.Offline() || (st.covers.BackgroundLeft() > 0 && st.covers.BackgroundOffline())) &&
					 cfg.covers_download)
			{
				const char* s = "Offline: covers next time";
				DrawText(W - 80 - TextWidth(s, 3), 112, s, 3, Rgb(170, 160, 210));
			}
			// hints
			ps5video::DarkenRect(0, H - 78, W, 78);
			const std::string sp = "      ";
			const std::string mic_hint = !ps5mic::Available() ? std::string()
				: ps5mic::Unmuted() ? std::string("Blow into the microphone") + sp
				: std::string("Microphone muted: press the mic button, then blow") + sp;
			const std::string hint = n > 0 && circ > 0.5f
				? mic_hint + "Touchpad hold: Clean" + sp + "Touchpad swipe down: Back" + sp + icon::Options + " Quit"
				: n > 0
				? std::string(icon::Cross) + " Play" + sp + icon::DpadLeftRight + " Browse" + sp + icon::DpadUpDown + " System" +
					sp + icon::L1 + " " + icon::R1 + " Skip 10" + sp + icon::L2 + " " + icon::R2 + " Turn" + sp + icon::Triangle +
					" Settings" + sp + icon::Square + (tabs[size_t(tab)] == kDriveTab ? " Add to collection" : " Get cover") + sp + icon::Circle +
					" Favorite" + sp + icon::L3 + " Style" + sp +
					// the touchpad, by style (user request): swipe up on a cartridge, sideways on a floppy; nothing on boxes
					(ShelfStyle() == 1 ? "Touchpad swipe up: Clean contacts" + sp
						: ShelfStyle() == 2 ? "Touchpad swipe: Shutter" + sp
						: (st.slots[size_t(sel)] && st.slots[size_t(sel)]->art && st.slots[size_t(sel)]->art->clamshell)
							? "Touchpad swipe left / right: Open / Close" + sp
						: (st.slots[size_t(sel)] && st.slots[size_t(sel)]->media == cart3d::Kind::Cartridge && !IsArabicEdition(st.games[size_t(sel)]) &&
							  ItemKind(*st.slots[size_t(sel)], 0) == cart3d::Kind::Box)
							? "Touchpad swipe right / left: Insert out / in" + sp : std::string()) +
					icon::Options + " Quit"
				: std::string(icon::DpadUpDown) + " System" + sp + icon::Triangle + " Settings" + sp + icon::Options + " Quit";
#if defined(MSX_ANDROID)
			(void)hint;
			{
				// the Thor: two short lines in the bottom screen's middle 1240 columns, its own buttons and the touch screen
				const std::string s2 = "    ";
				std::string l1, l2;
				const auto* slot = n > 0 ? st.slots[size_t(sel)].get() : nullptr;
				if (n > 0 && circ > 0.5f)
				{
					l1 = (ps5mic::Available() ? std::string("Blow into the mic") + s2 : std::string()) + "Hold: Clean" + s2 + "Swipe down: Back";
					l2 = std::string(icon::Options) + " Quit";
				}
				else if (n > 0)
				{
					const std::string swipe = ShelfStyle() == 1 ? "Swipe up: Clean"
						: ShelfStyle() == 2 ? "Swipe: Shutter"
						: (slot && slot->art && slot->art->clamshell) ? "Swipe left / right: Open / Close"
						: (slot && slot->media == cart3d::Kind::Cartridge && !IsArabicEdition(st.games[size_t(sel)]) &&
							  ItemKind(*slot, 0) == cart3d::Kind::Box)
							? "Swipe right / left: Insert out / in" : "";
					l1 = std::string(icon::Cross) + " / Tap  Play" + s2 + icon::DpadLeftRight + " Browse" + s2 + icon::DpadUpDown + " System" + s2 +
						icon::L1 + " " + icon::R1 + " Skip 10" + s2 + icon::L2 + " " + icon::R2 + " Turn";
					l2 = std::string(icon::Triangle) + " Settings" + s2 + icon::Square + " Cover" + s2 + icon::Circle + " Favorite" + s2 +
						icon::L3 + " Style" + s2 + (swipe.empty() ? std::string() : swipe + s2) + icon::Options + " Quit";
				}
				else
					l1 = std::string(icon::DpadUpDown) + " System" + s2 + icon::Triangle + " Settings" + s2 + icon::Options + " Quit";
				constexpr int kX0 = (W - 1240) / 2, kW = 1240 - 40;
				ps5video::DarkenRect(0, H - 116, W, 38); // (the bar: two lines tall)
				const bool two = !l2.empty();
				int hs = 3;
				while (hs > 1 && (TextWidth(l1.c_str(), hs) > kW || (two && TextWidth(l2.c_str(), hs) > kW)))
					hs--;
				const int lh = hs == 3 ? 46 : 36;
				const int y0 = two ? H - 22 - 2 * lh : H - 60;
				DrawText(kX0 + 20 + (kW - TextWidth(l1.c_str(), hs)) / 2, y0, l1.c_str(), hs, Rgb(205, 200, 228));
				if (two)
					DrawText(kX0 + 20 + (kW - TextWidth(l2.c_str(), hs)) / 2, y0 + lh, l2.c_str(), hs, Rgb(205, 200, 228));
			}
#else
			const int hs = TextWidth(hint.c_str(), 3) <= W - 60 ? 3 : 2; // (the long line: smaller)
			DrawText((W - TextWidth(hint.c_str(), hs)) / 2, hs == 3 ? H - 60 : H - 52, hint.c_str(), hs, Rgb(205, 200, 228));
#endif

		}
		ps5video::Present(0, 0, 0, 0, true);
	}
}
} // namespace fe
