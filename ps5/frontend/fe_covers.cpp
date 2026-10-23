// MSX PS5 frontend: covers (fe_covers.h).
// SPDX-License-Identifier: MIT

#include "fe_covers.h"

#include "fe_art.h"

#include "fe_coverworker.h"
#include "fe_text.h"

#include "OrbisPaths.h"
#include "ProsperoCrash.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <ctime>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
// a cover is a few hundred pixels: a bigger image (a broken or hostile file) is refused before stb_image
// allocates for it (its default limit, 2^24 pixels a side, would let one PNG ask for gigabytes)
#define STBI_MAX_DIMENSIONS 8192
#include "third_party/stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#include "third_party/stb_image_resize2.h"

namespace fe
{
namespace
{
// (below: a box of its own over the made one)
std::shared_ptr<const cart3d::Art> WithOwnBox(std::shared_ptr<const cart3d::Art> made, const fe::GameInfo& g);
} // namespace
namespace
{
constexpr int kLoadRadius = 12; // textures kept around the selection
constexpr long kMissingRetrySeconds = 30L * 24 * 3600;

// the status line's counter: the worker counts down, Refetch (another thread) up
void DecrementToZero(std::atomic<int>& a)
{
	int v = a.load();
	while (v > 0 && !a.compare_exchange_weak(v, v - 1))
	{
	}
}

const char* const kDefaultUrl =
	"https://raw.githubusercontent.com/libretro-thumbnails/${repo}/master/Named_Boxarts/${name}.png";

std::string CoverUrlTemplate()
{
	const char* env = getenv("GENPLUS_COVER_URL"); // the host tests point this at a local server
	return env && *env ? env : kDefaultUrl;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
	{
		out.insert(out.end(), buf, buf + n);
		if (out.size() > (32u << 20))
			break;
	}
	fclose(f);
	return !out.empty();
}

bool WriteFileAtomic(const std::string& path, const std::vector<uint8_t>& data)
{
	const std::string tmp = path + ".part";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

bool NonEmptyFile(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

std::string FindWithExts(const std::string& base)
{
	static const char* const exts[] = {".png", ".jpg", ".jpeg", ".PNG", ".JPG"};
	for (const char* e : exts)
		if (NonEmptyFile(base + e))
			return base + e;
	return "";
}

// Square on the shelf: "<cover>.refetch" asks for the cover again. The cover itself stays where it is until a new
// one is saved over it (atomically), so a refetch that fails (offline, 404, no restart) never loses it.
std::string RefetchMarker(const std::string& cover)
{
	return cover.substr(0, cover.size() - 4) + ".refetch";
}

bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}

bool RecentlyMissing(const std::string& marker)
{
	struct stat st = {};
	if (stat(marker.c_str(), &st) != 0)
		return false;
	return time(nullptr) - st.st_mtime < kMissingRetrySeconds;
}

uint32_t Pack(int r, int g, int b)
{
	return 0xff000000u | (uint32_t(std::clamp(b, 0, 255)) << 16) | (uint32_t(std::clamp(g, 0, 255)) << 8) |
		   uint32_t(std::clamp(r, 0, 255));
}

// Row-major RGBA (h rows of w) -> a CoverTex with 3 levels, column-major.
std::shared_ptr<CoverTex> MakeTex(const uint32_t* rgba, int w, int h)
{
	auto tex = std::make_shared<CoverTex>();
	std::vector<uint32_t> level(rgba, rgba + size_t(w) * h);
	int lw = w, lh = h;
	uint64_t sr = 0, sg = 0, sb = 0;
	for (int l = 0; l < CoverTex::kLevels; l++)
	{
		tex->w[l] = lw;
		tex->h[l] = lh;
		tex->px[l].resize(size_t(lw) * lh);
		for (int y = 0; y < lh; y++)
			for (int x = 0; x < lw; x++)
				tex->px[l][size_t(x) * lh + y] = level[size_t(y) * lw + x];
		if (l + 1 == CoverTex::kLevels)
		{
			for (uint32_t p : level)
			{
				sr += p & 0xff;
				sg += (p >> 8) & 0xff;
				sb += (p >> 16) & 0xff;
			}
			const uint64_t n = std::max<uint64_t>(1, level.size());
			tex->average = Pack(int(sr / n), int(sg / n), int(sb / n));
			break;
		}
		// 2x2 box down
		const int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
		std::vector<uint32_t> next(size_t(nw) * nh);
		for (int y = 0; y < nh; y++)
			for (int x = 0; x < nw; x++)
			{
				const uint32_t* r0 = &level[size_t(std::min(2 * y, lh - 1)) * lw];
				const uint32_t* r1 = &level[size_t(std::min(2 * y + 1, lh - 1)) * lw];
				const int x0 = std::min(2 * x, lw - 1), x1 = std::min(2 * x + 1, lw - 1);
				const uint32_t a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
				uint32_t out = 0xff000000u;
				for (int sh = 0; sh < 24; sh += 8)
				{
					const uint32_t v = (((a >> sh) & 0xff) + ((b >> sh) & 0xff) + ((c >> sh) & 0xff) + ((d >> sh) & 0xff) + 2) / 4;
					out |= v << sh;
				}
				next[size_t(y) * nw + x] = out;
			}
		level.swap(next);
		lw = nw;
		lh = nh;
	}
	return tex;
}

std::shared_ptr<CoverTex> Decode(const std::vector<uint8_t>& file, const GameInfo& g, int turns, const Shots* shots)
{
	int w = 0, h = 0, comp = 0;
	stbi_uc* img = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &comp, 4);
	if (!img || w < 8 || h < 8)
	{
		if (img)
			stbi_image_free(img);
		return nullptr;
	}
	turns &= 3;
	// the box's picture (always portrait): the cover as it was stored, a landscape one turned a quarter
	std::vector<uint32_t> box;
	int bw = 0, bh = 0;
	{
		const uint32_t* src = reinterpret_cast<const uint32_t*>(img);
		std::vector<uint32_t> portrait;
		int pw = w, ph = h;
		if (w > h)
		{
			pw = h, ph = w;
			portrait.resize(size_t(pw) * ph);
			for (int y = 0; y < ph; y++)
				for (int x = 0; x < pw; x++)
					portrait[size_t(y) * pw + x] = src[size_t(h - 1 - x) * w + y]; // a quarter clockwise
			src = portrait.data();
		}
		if (turns || w > h)
		{
			bh = kCoverTexH, bw = int(std::lround(bh * std::clamp(float(pw) / ph, 0.5f, 1.0f))) & ~3;
			box.resize(size_t(bw) * bh);
			stbir_resize_uint8_srgb(reinterpret_cast<const unsigned char*>(src), pw, ph, 0, reinterpret_cast<unsigned char*>(box.data()),
				bw, bh, 0, STBIR_RGBA);
			for (uint32_t& px : box)
				px |= 0xff000000u;
		}
	}
	if (turns)
	{
		// turned upright: `turns` quarters clockwise
		const int rw = (turns & 1) ? h : w, rh = (turns & 1) ? w : h;
		stbi_uc* rot = static_cast<stbi_uc*>(malloc(size_t(rw) * rh * 4));
		if (rot)
		{
			const uint32_t* src = reinterpret_cast<const uint32_t*>(img);
			uint32_t* dst = reinterpret_cast<uint32_t*>(rot);
			for (int y = 0; y < rh; y++)
				for (int x = 0; x < rw; x++)
				{
					const int sx = turns == 1 ? y : turns == 2 ? w - 1 - x : w - 1 - y;
					const int sy = turns == 1 ? h - 1 - x : turns == 2 ? h - 1 - y : x;
					dst[size_t(y) * rw + x] = src[size_t(sy) * w + sx];
				}
			stbi_image_free(img);
			img = rot; // (stbi_image_free is free())
			w = rw, h = rh;
		}
	}
	// level 0: kCoverTexH tall, the art's own aspect (kept between 0.5:1 and 1.6:1)
	const float aspect = std::clamp(float(w) / float(h), 0.5f, 1.6f);
	const int th = kCoverTexH;
	const int tw = int(std::lround(th * aspect)) & ~3;
	std::vector<uint32_t> out(size_t(tw) * th);
	stbir_resize_uint8_srgb(img, w, h, 0, reinterpret_cast<unsigned char*>(out.data()), tw, th, 0, STBIR_RGBA);
	stbi_image_free(img);
	for (uint32_t& p : out)
		p |= 0xff000000u; // covers are opaque
	auto tex = MakeTex(out.data(), tw, th);
	tex->art = WithOwnBox(MakeArt(out.data(), tw, th, g, shots, box.empty() ? nullptr : box.data(), bw, bh), g);
	tex->media = MediaOf(g);
	return tex;
}

// The game's screens that are in the cover folder (ScreensFor), decoded.
Shots LoadShots(const GameInfo& g)
{
	Shots shots;
	const std::string covers = OrbisDir("covers");
	bool first = true;
	for (const WantedCover& w : ScreensFor(g))
	{
		const bool is_title = first && w.file.size() > 10 && w.file.compare(w.file.size() - 10, 10, ".title.png") == 0;
		first = false;
		std::vector<uint8_t> file;
		if (!ReadFile(covers + "/" + w.file, file))
			continue;
		int sw = 0, sh = 0, comp = 0;
		stbi_uc* img = stbi_load_from_memory(file.data(), int(file.size()), &sw, &sh, &comp, 4);
		if (!img)
			continue;
		Shot shot;
		shot.w = sw, shot.h = sh;
		shot.px.assign(reinterpret_cast<uint32_t*>(img), reinterpret_cast<uint32_t*>(img) + size_t(sw) * sh);
		stbi_image_free(img);
		for (uint32_t& px : shot.px)
			px |= 0xff000000u;
		if (is_title)
			shots.title = std::move(shot);
		else
			shots.snaps.push_back(std::move(shot));
	}
	return shots;
}

} // namespace

bool LoadMainScreenshot(const GameInfo& g, std::vector<uint32_t>& px, int& w, int& h)
{
	const std::string covers = OrbisDir("covers");
	std::vector<std::string> files = {covers + "/shots/" + g.file_base + ".png"};
	std::string title;
	for (const WantedCover& wc : ScreensFor(g))
	{
		const bool is_title = wc.file.size() > 10 && wc.file.compare(wc.file.size() - 10, 10, ".title.png") == 0;
		if (is_title)
			title = covers + "/" + wc.file;
		else
			files.push_back(covers + "/" + wc.file);
	}
	if (!title.empty())
		files.push_back(title);
	for (const std::string& f : files)
	{
		std::vector<uint8_t> file;
		if (!ReadFile(f, file))
			continue;
		int comp = 0;
		stbi_uc* img = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &comp, 4);
		if (!img)
			continue;
		px.assign(reinterpret_cast<uint32_t*>(img), reinterpret_cast<uint32_t*>(img) + size_t(w) * h);
		stbi_image_free(img);
		for (uint32_t& p : px)
			p |= 0xff000000u;
		return true;
	}
	return false;
}

namespace
{
std::shared_ptr<CoverTex> Placeholder(const GameInfo& g, const Shots* shots)
{
	const int w = 720, h = kCoverTexH;
	std::vector<uint32_t> px(size_t(w) * h);
	for (int y = 0; y < h; y++)
	{
		const float t = float(y) / h;
		const uint32_t c = Pack(int(52 - 30 * t), int(42 - 24 * t), int(96 - 52 * t));
		std::fill(px.begin() + size_t(y) * w, px.begin() + size_t(y + 1) * w, c);
	}
	// frame
	const uint32_t edge = Pack(150, 125, 255);
	for (int i = 0; i < 6; i++)
		for (int x = 0; x < w; x++)
		{
			px[size_t(i) * w + x] = edge;
			px[size_t(h - 1 - i) * w + x] = edge;
		}
	for (int y = 0; y < h; y++)
		for (int i = 0; i < 6; i++)
		{
			px[size_t(y) * w + i] = edge;
			px[size_t(y) * w + w - 1 - i] = edge;
		}
	// four coloured buttons, top right
	const uint32_t dots[4] = {Pack(225, 60, 70), Pack(240, 200, 60), Pack(70, 180, 90), Pack(70, 110, 230)};
	const int cx[4] = {w - 130, w - 95, w - 60, w - 95}, cy[4] = {80, 50, 80, 110};
	for (int k = 0; k < 4; k++)
		for (int y = -14; y <= 14; y++)
			for (int x = -14; x <= 14; x++)
				if (x * x + y * y <= 196)
					px[size_t(cy[k] + y) * w + cx[k] + x] = dots[k];

	Canvas canvas{px.data(), w, h, w};
	// the title, word-wrapped, up to 4 lines
	std::vector<std::string> lines;
	std::string cur;
	size_t pos = 0;
	const std::string& t = g.title;
	while (pos <= t.size())
	{
		size_t sp = t.find(' ', pos);
		if (sp == std::string::npos)
			sp = t.size();
		const std::string word = t.substr(pos, sp - pos);
		const std::string tryline = cur.empty() ? word : cur + " " + word;
		if (!cur.empty() && TextWidth(tryline.c_str(), 5) > w - 80)
		{
			lines.push_back(cur);
			cur = word;
		}
		else
			cur = tryline;
		pos = sp + 1;
	}
	if (!cur.empty())
		lines.push_back(cur);
	if (lines.size() > 4)
	{
		lines.resize(4);
		lines[3] = FitText(lines[3] + "...", 5, w - 80);
	}
	int y = 190 - int(lines.size()) * 28;
	for (const std::string& l : lines)
	{
		const std::string fit = FitText(l, 5, w - 80);
		DrawTextOn(canvas, (w - TextWidth(fit.c_str(), 5)) / 2, y, fit.c_str(), 5, Pack(240, 240, 250));
		y += 58;
	}
	std::string sub = Info(g.system).name;
	for (char& c : sub)
		c = char(toupper(uint8_t(c)));
	DrawTextOn(canvas, (w - TextWidth(sub.c_str(), 3)) / 2, h - 80, sub.c_str(), 3, Pack(170, 160, 210));
	auto tex = MakeTex(px.data(), w, h);
	tex->source = "placeholder";
	tex->art = WithOwnBox(MakeArt(px.data(), w, h, g, shots), g);
	tex->media = MediaOf(g);
	return tex;
}
} // namespace

std::string ThumbnailName(const std::string& nointro)
{
	std::string s = nointro;
	for (char& c : s)
		if (strchr("&*/:`<>?\\|\"", c))
			c = '_';
	return s;
}

namespace
{
std::atomic<bool> g_shelf_downloads{true};

#include "msx_thumbs.inc"

// 2026-10-16: a box of its own (the Arabic collection's printed boxes): covers/boxes/<ROM name>.front / .back / .spine
// (.png or .jpg) take the place of the box faces made from the cover. Each is scaled to the size of the made ones.
void OwnBox(cart3d::Art& art, const fe::GameInfo& g)
{
	const std::string base = OrbisDir("covers") + "/boxes/" + g.file_base;
	struct Part
	{
		const char* name;
		cart3d::TexPtr* tex;
		int w, h; // (0 w: the picture's own aspect at height h)
	};
	const Part parts[5] = {{"front", &art.front, 0, fe::kCoverTexH}, {"back", &art.back, 0, fe::kCoverTexH}, {"spine", &art.spine, 112, 768},
		{"top", &art.top, 1024, 256}, {"rear", &art.rear, 896, 768}};
	{
		// its cartridge's shape ("tall": the Quran's)
		std::vector<uint8_t> shape;
		if (ReadFile(base + ".cart", shape) && std::string(shape.begin(), shape.end()).find("tall") != std::string::npos)
			art.tall = true;
	}
	{
		// its plastic's colour, "rrggbb" (sRGB): the black plastic's albedo scaled to it
		std::vector<uint8_t> file;
		unsigned c = 0;
		if (ReadFile(base + ".plastic", file) && !file.empty())
		{
			file.push_back(0);
			if (std::sscanf(reinterpret_cast<const char*>(file.data()), "%6x", &c) == 1)
			{
				for (int i = 0; i < 3; i++)
				{
					const float s = float((c >> (16 - 8 * i)) & 0xff) / 255.0f;
					const float lin = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
					art.tint[i] = lin / 0.008f;
				}
				art.tinted = true;
			}
		}
	}
	for (const Part& p : parts)
	{
		std::vector<uint8_t> file;
		if (!ReadFile(base + "." + p.name + ".png", file) && !ReadFile(base + "." + p.name + ".jpg", file))
			continue;
		int w = 0, h = 0, comp = 0;
		stbi_uc* img = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &comp, 4);
		if (!img)
			continue;
		const int th = p.h, tw = p.w ? p.w : (int(std::lround(th * std::clamp(float(w) / float(h), 0.3f, 1.6f))) & ~3);
		std::vector<uint32_t> px(size_t(tw) * th);
		stbir_resize_uint8_srgb(img, w, h, 0, reinterpret_cast<unsigned char*>(px.data()), tw, th, 0, STBIR_RGBA);
		stbi_image_free(img);
		for (uint32_t& q : px)
			q |= 0xff000000u;
		*p.tex = cart3d::Texture::FromPixels(px.data(), tw, th);
		if (p.tex == &art.spine)
			art.clamshell = true; // (a printed box of its own: Al Alamiah's black plastic clamshell)
	}
}
std::shared_ptr<const cart3d::Art> WithOwnBox(std::shared_ptr<const cart3d::Art> made, const fe::GameInfo& g)
{
	if (!made)
		return made;
	std::shared_ptr<cart3d::Art> art = std::const_pointer_cast<cart3d::Art>(made); // (made non-const by MakeArt)
	OwnBox(*art, g);
	return art;
}

// "Pay Load (Japan) (Alt 1)" -> "payload": the title without its (tags) and [tags], letters and digits, lower case.
std::string TitleKey(const std::string& name)
{
	std::string out;
	int depth = 0;
	for (size_t i = 0; i < name.size(); i++)
	{
		const char c = name[i];
		if (c == '(' || c == '[')
			depth++;
		else if ((c == ')' || c == ']') && depth > 0)
			depth--;
		else if (depth == 0)
		{
			if (c == '&')
				out += "and";
			else if (std::isalnum(static_cast<unsigned char>(c)))
				out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}
	return out;
}

// The first tag, usually the region: "(Japan)" of "Pay Load (Japan) (Alt 1)"; "" when there is none.
std::string FirstTag(const std::string& name)
{
	const size_t open = name.find('(');
	const size_t close = open == std::string::npos ? std::string::npos : name.find(')', open);
	return close == std::string::npos ? std::string() : name.substr(open, close - open + 1);
}

struct Thumb
{
	char repo;   // '1' MSX, '2' MSX2
	char folder; // 'B' box art, 'T' title screen, 'S' screenshot
	const char* name;
};

// The built-in list by title key (made once).
const std::vector<Thumb>* ThumbsFor(const std::string& key)
{
	static const auto* index = [] {
		auto* m = new std::unordered_map<std::string, std::vector<Thumb>>();
		for (const char* e : kMsxThumbs)
			(*m)[TitleKey(e + 2)].push_back({e[0], e[1], e + 2});
		return m;
	}();
	const auto it = index->find(key);
	return it == index->end() ? nullptr : &it->second;
}
} // namespace

void SetShelfDownloads(bool on)
{
	g_shelf_downloads = on;
}

bool ShelfDownloads()
{
	return g_shelf_downloads;
}

std::string CoverUrlFor(const GameInfo& g)
{
	// MSX PS5 (2026-10-10, AI-assisted): the addresses to try, tab-separated (fe_coverfetch tries them in turn).
	// libretro-thumbnails names most MSX covers after the main dump only: "Arkanoid (Japan) (Alt 1)" and
	// "Aleste (Japan) (Translated En)" are found as "Arkanoid (Japan)" / "Aleste (Japan)"; and a game may sit in
	// the MSX2 collection rather than the MSX one (or the other way round).
	std::vector<std::string> names = {ThumbnailName(g.nointro)};
	const size_t open = g.nointro.find(" (");
	if (open != std::string::npos)
	{
		const size_t close = g.nointro.find(')', open);
		if (close != std::string::npos && close + 1 < g.nointro.size())
			names.push_back(ThumbnailName(g.nointro.substr(0, close + 1))); // title + its first (region) tag
	}
	std::vector<std::string> repos = {Info(g.system).thumbs};
	if (g.system == System::Msx || g.system == System::Msx2)
		repos.push_back(Info(g.system == System::Msx ? System::Msx2 : System::Msx).thumbs);
	std::string all;
	auto add = [&all](const std::string& repo, const char* folder, const std::string& name) {
		std::string url = CoverUrlTemplate();
		size_t p = url.find("${repo}");
		if (p != std::string::npos)
			url.replace(p, 7, repo);
		p = url.find("Named_Boxarts");
		if (p != std::string::npos && std::strcmp(folder, "Named_Boxarts") != 0)
			url.replace(p, 13, folder);
		p = url.find("${name}");
		if (p != std::string::npos)
			url.replace(p, 7, UrlEncode(name));
		if (all.find(url) == std::string::npos)
			all += (all.empty() ? "" : "\t") + url;
	};
	for (const std::string& repo : repos)
		for (const std::string& name : names)
			add(repo, "Named_Boxarts", name);
	// 2026-10-10: by title in the built-in list of the collection's images (msx_thumbs.inc): box art of the same region
	// first, then any box art, then the title screen, then a screenshot; at most six more addresses.
	if (const std::vector<Thumb>* found = ThumbsFor(TitleKey(g.nointro)))
	{
		const std::string region = FirstTag(g.nointro);
		std::vector<const Thumb*> order;
		for (const char folder : {'B', 'T', 'S'})
			for (int same = 1; same >= 0; same--)
				for (const Thumb& t : *found)
					if (t.folder == folder && (FirstTag(t.name) == region) == (same == 1))
						order.push_back(&t);
		int extra = 0;
		for (const Thumb* t : order)
		{
			if (extra++ >= 6)
				break;
			add(t->repo == '1' ? "Microsoft_-_MSX" : "Microsoft_-_MSX2",
				t->folder == 'B' ? "Named_Boxarts" : t->folder == 'T' ? "Named_Titles" : "Named_Snaps", t->name);
		}
	}
	return all;
}

std::vector<WantedCover> ScreensFor(const GameInfo& g)
{
	std::vector<WantedCover> out;
	if (g.nointro.empty())
		return out;
	const std::string folder = Info(g.system).folder;
	auto url = [](const std::string& repo, const char* kind, const std::string& name) {
		std::string u = CoverUrlTemplate();
		size_t p = u.find("${repo}");
		if (p != std::string::npos)
			u.replace(p, 7, repo);
		p = u.find("Named_Boxarts");
		if (p != std::string::npos)
			u.replace(p, 13, kind);
		p = u.find("${name}");
		if (p != std::string::npos)
			u.replace(p, 7, UrlEncode(name));
		return u;
	};
	const std::string region = FirstTag(g.nointro);
	const std::vector<Thumb>* found = ThumbsFor(TitleKey(g.nointro));
	// the title screen: the same region's first
	if (found)
		for (int same = 1; same >= 0 && (out.empty()); same--)
			for (const Thumb& t : *found)
				if (t.folder == 'T' && (FirstTag(t.name) == region) == (same == 1))
				{
					out.push_back({folder + "/" + ThumbnailName(t.name) + ".title.png",
						url(t.repo == '1' ? "Microsoft_-_MSX" : "Microsoft_-_MSX2", "Named_Titles", t.name)});
					break;
				}
	// the in-game pictures: three of its versions' (the same region's first)
	int snaps = 0;
	if (found)
		for (int same = 1; same >= 0; same--)
			for (const Thumb& t : *found)
				if (snaps < 3 && t.folder == 'S' && (FirstTag(t.name) == region) == (same == 1))
				{
					out.push_back({folder + "/" + ThumbnailName(t.name) + ".snap.png",
						url(t.repo == '1' ? "Microsoft_-_MSX" : "Microsoft_-_MSX2", "Named_Snaps", t.name)});
					snaps++;
				}
	if (snaps == 0)
	{
		// not in the built-in list: by its own name, in both collections
		const std::string name = ThumbnailName(g.nointro);
		out.push_back({folder + "/" + name + ".snap.png",
			url("Microsoft_-_MSX", "Named_Snaps", g.nointro) + "\t" + url("Microsoft_-_MSX2", "Named_Snaps", g.nointro)});
	}
	return out;
}

std::string OwnCover(const GameInfo& g)
{
	// your own: covers/<system folder>/<ROM name>.png|jpg, or covers/<ROM name>.png|jpg
	const std::string covers = OrbisDir("covers");
	std::string own = FindWithExts(covers + "/" + Info(g.system).folder + "/" + g.file_base);
	if (own.empty())
		own = FindWithExts(covers + "/" + g.file_base);
	return own;
}

std::string CoverFileFor(const GameInfo& g)
{
	return std::string(Info(g.system).folder) + "/" + ThumbnailName(g.nointro) + ".png";
}

std::string WantedListPath()
{
	return OrbisDir("covers") + "/wanted.txt";
}

std::vector<WantedCover> MissingCovers(const std::vector<GameInfo>& games)
{
	std::vector<WantedCover> out;
	const std::string covers = OrbisDir("covers");
	// 2026-10-10: once, the 404 marks left from before covers were found by title go, so those games are asked again.
	const std::string marker = covers + "/.found-by-title";
	if (!Exists(marker))
	{
		int dropped = 0;
		for (const char* sub : {"MSX", "MSX2", "Disks"})
			if (DIR* d = opendir((covers + "/" + sub).c_str()))
			{
				while (struct dirent* e = readdir(d))
				{
					const size_t n = std::strlen(e->d_name);
					if (n > 8 && std::strcmp(e->d_name + n - 8, ".missing") == 0 &&
						unlink((covers + "/" + sub + "/" + e->d_name).c_str()) == 0)
						dropped++;
				}
				closedir(d);
			}
		if (FILE* f = std::fopen(marker.c_str(), "w"))
			std::fclose(f);
		std::printf("[covers] covers by title: %d old 'missing' marks dropped, those games are asked again\n", dropped);
	}
	std::vector<WantedCover> screens; // (after all the covers)
	for (const GameInfo& g : games)
	{
		if (g.nointro.empty())
			continue;
		for (const WantedCover& w : ScreensFor(g))
		{
			const std::string path = covers + "/" + w.file;
			if (NonEmptyFile(path) || RecentlyMissing(path.substr(0, path.size() - 4) + ".missing"))
				continue;
			bool dup = false;
			for (const WantedCover& o : screens)
				dup = dup || o.file == w.file;
			if (!dup)
				screens.push_back(w);
		}
		const std::string file = CoverFileFor(g);
		const std::string cache = covers + "/" + file;
		if (!Exists(RefetchMarker(cache)) &&
			(NonEmptyFile(cache) || RecentlyMissing(cache.substr(0, cache.size() - 4) + ".missing")))
			continue;
		const std::string own = OwnCover(g);
		if (!own.empty() && own != cache)
			continue; // your own cover (a ROM named as its official name shares the downloaded cover's file)
		const std::string dir = g.path.substr(0, g.path.find_last_of('/'));
		if (!FindWithExts(dir + "/" + g.file_base).empty())
			continue; // a cover beside the ROM
		bool dup = false;
		for (const WantedCover& w : out)
			dup = dup || w.file == file;
		if (!dup)
			out.push_back({file, CoverUrlFor(g)});
	}
	out.insert(out.end(), screens.begin(), screens.end());
	return out;
}

void WriteWantedList(const std::vector<WantedCover>& wanted)
{
	std::string text;
	for (const WantedCover& w : wanted)
		text += w.file + "\t" + w.url + "\n";
	WriteFileAtomic(WantedListPath(), std::vector<uint8_t>(text.begin(), text.end()));
	OrbisLog("[covers] wanted list: %zu cover(s) for the next prefetch", wanted.size());
}

std::string CoverService::CachePath(int i) const
{
	const GameInfo& g = m_games[size_t(i)];
	if (g.nointro.empty())
		return "";
	return OrbisDir("covers") + "/" + CoverFileFor(g);
}

bool CoverService::NeedsDownload(int i) const
{
	const GameInfo& g = m_games[size_t(i)];
	const std::string cache = CachePath(i);
	if (cache.empty())
		return false;
	const std::string own = OwnCover(g);
	if (!own.empty() && own != cache)
		return false; // your own cover, named after the ROM (not the downloaded one, when the names are the same)
	const std::string dir = g.path.substr(0, g.path.find_last_of('/'));
	if (!FindWithExts(dir + "/" + g.file_base).empty())
		return false; // a picture beside the ROM is used before a download
	if (Exists(RefetchMarker(cache)))
		return true; // asked for again (Square)
	return !NonEmptyFile(cache) && !RecentlyMissing(cache.substr(0, cache.size() - 4) + ".missing");
}

void CoverService::Start(const std::vector<GameInfo>& games, bool allow_download, bool background)
{
	Stop();
	m_games = games;
	m_allow_download = allow_download;
	m_background = background && !allow_download;
	m_placeholder.assign(games.size(), 0);
	m_refetching.clear();
	m_prio_focus = -1;
	m_last_prio.clear();
	m_bg_left = 0;
	m_bg_offline = false;
	m_quit = false;
	m_state.assign(games.size(), 0);
	m_fetched.assign(games.size(), 0);
	m_ready.clear();
	m_downloaded = 0;
	int need = 0;
	if (allow_download)
		for (size_t i = 0; i < games.size(); i++)
			need += NeedsDownload(int(i)) ? 1 : 0;
	m_to_download = need;
	OrbisLog("[covers] %zu game(s), %d cover(s) to download%s", games.size(), need,
		allow_download ? "" : m_background ? " (the helper downloads them in the background)" : " (downloads off)");
	m_thread = ps5::BigThread([this] { Run(); }, 8 * 1024 * 1024);
}

void CoverService::Stop()
{
	if (!m_thread.joinable())
		return;
	m_quit = true;
	m_http.Abort();
	m_wake.notify_all();
	m_thread.join();
	m_http.Term();
}

void CoverService::Collect(std::vector<CoverPtr>& slots, int keep_radius)
{
	if (slots.size() != m_games.size())
		slots.assign(m_games.size(), nullptr);
	std::lock_guard<std::mutex> lock(m_lock);
	while (!m_ready.empty())
	{
		auto& r = m_ready.front();
		if (r.first >= 0 && size_t(r.first) < slots.size())
			slots[size_t(r.first)] = r.second;
		m_ready.pop_front();
	}
	const int focus = m_focus;
	for (size_t i = 0; i < slots.size(); i++)
		if (slots[i] && std::abs(int(i) - focus) > keep_radius)
		{
			slots[i] = nullptr;
			if (m_state[i] == 2)
				m_state[i] = 0;
		}
}

void CoverService::Reload(int i)
{
	std::lock_guard<std::mutex> lock(m_lock);
	if (i < 0 || size_t(i) >= m_state.size())
		return;
	m_state[size_t(i)] = 0; // (one being loaded now is dropped when it is done)
	m_wake.notify_one();
}

// ---- covers stored sideways ----------------------------------------------------------------------------------
namespace
{
// libretro-thumbnails' MSX box art that is a landscape box scanned upright (titles without tags, lower case):
// quarter turns clockwise that put it upright
const struct
{
	const char* title;
	int turns;
} kSideways[] = {
	{"hades no monsho", 1}, {"haunted boynight", 1}, {"hudson 3d golf", 1}, {"king knight", 1},
	{"knightmare iii - shalom", 1}, {"koneko no daibouken - catboy", 3}, {"machinegun joe vs the mafia", 1},
	{"roger rubbish", 3}, {"sea hunter", 1}, {"the spider", 1}, {"suparobo", 3}, {"super cross force", 3},
	{"tzr - grand prix rider", 3},
};

std::string TurnKey(const GameInfo& g)
{
	std::string t = gamedb::Title(g.nointro.empty() ? g.file_base : g.nointro);
	for (char& c : t)
		c = char(tolower(uint8_t(c)));
	return t;
}
std::string TurnsPath()
{
	return OrbisRoot() + "/covers/turns.txt";
}
std::mutex s_turns_lock;
std::unordered_map<std::string, int>* s_turns = nullptr; // your own turns, by TurnKey
void LoadTurns()
{
	if (s_turns)
		return;
	s_turns = new std::unordered_map<std::string, int>();
	if (FILE* f = fopen(TurnsPath().c_str(), "r"))
	{
		char line[512];
		while (fgets(line, sizeof(line), f))
		{
			std::string l(line);
			while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
				l.pop_back();
			const size_t eq = l.rfind('=');
			if (eq != std::string::npos && eq > 0)
				(*s_turns)[l.substr(0, eq)] = atoi(l.c_str() + eq + 1) & 3;
		}
		fclose(f);
	}
}
} // namespace

int CoverTurns(const GameInfo& g, bool downloaded)
{
	const std::string key = TurnKey(g);
	int t = 0;
	if (downloaded)
		for (const auto& s : kSideways)
			if (key == s.title)
				t = s.turns;
	std::lock_guard<std::mutex> lock(s_turns_lock);
	LoadTurns();
	const auto it = s_turns->find(key);
	return (t + (it != s_turns->end() ? it->second : 0)) & 3;
}

void TurnCover(const GameInfo& g)
{
	std::lock_guard<std::mutex> lock(s_turns_lock);
	LoadTurns();
	int& t = (*s_turns)[TurnKey(g)];
	t = (t + 1) & 3;
	if (FILE* f = fopen(TurnsPath().c_str(), "w"))
	{
		fprintf(f, "# MSX PS5: covers turned on the shelf (R3): <title>=<quarter turns clockwise>\n");
		for (const auto& e : *s_turns)
			if (e.second)
				fprintf(f, "%s=%d\n", e.first.c_str(), e.second);
		fclose(f);
	}
	OrbisLog("[covers] %s turned: %d", TurnKey(g).c_str(), t);
}

void CoverService::Refetch(int i)
{
	if (i < 0 || size_t(i) >= m_games.size())
		return;
	const std::string cache = CachePath(i);
	if (cache.empty())
		return;
	// the cover stays: the marker asks for it again (here, or through the prefetch at the next start)
	if (FILE* f = fopen(RefetchMarker(cache).c_str(), "w"))
		fclose(f);
	unlink((cache.substr(0, cache.size() - 4) + ".missing").c_str());
	if (NeedsDownload(i))
		m_to_download++;
	struct stat st = {};
	const long long mtime = stat(cache.c_str(), &st) == 0 ? (long long)st.st_mtime : -1;
	std::lock_guard<std::mutex> lock(m_lock);
	m_fetched[size_t(i)] = 0;
	if (m_state[size_t(i)] == 2)
		m_state[size_t(i)] = 0;
	if (m_background)
		m_refetching[i] = mtime; // the helper's new file replaces the card when it lands (PollBackground)
	m_wake.notify_one();
}

bool CoverService::Download(int i)
{
	const GameInfo& g = m_games[size_t(i)];
	const std::string cache = CachePath(i);
	if (cache.empty())
		return false;
	const std::string marker = cache.substr(0, cache.size() - 4) + ".missing";
	if (m_offline || RecentlyMissing(marker))
		return false;
	const std::string url = CoverUrlFor(g);
	std::vector<uint8_t> data;
	GENPLUS_STAGE(Cover, "http get cover");
	const int status = FetchCoverUrl(m_http, url, ThumbnailName(g.nointro), data);
	if (status == 200)
	{
		OrbisMkdirs(cache.substr(0, cache.find_last_of('/')));
		if (WriteFileAtomic(cache, data))
		{
			unlink(RefetchMarker(cache).c_str()); // the new cover is in place
			m_downloaded++;
			return true;
		}
		OrbisLog("[covers] can't write %s", cache.c_str());
		return false;
	}
	if (status == 404)
	{
		unlink(RefetchMarker(cache).c_str()); // the server has none: the cover there (if any) stays
		FILE* f = fopen(marker.c_str(), "w");
		if (f)
		{
			fprintf(f, "%s\n", url.c_str());
			fclose(f);
		}
	}
	if (status == -1 && m_http.Offline())
		m_offline = true;
	return false;
}

CoverPtr CoverService::Load(int i, bool* downloaded)
{
	const GameInfo& g = m_games[size_t(i)];
	*downloaded = false;
	struct Cand
	{
		std::string path;
		const char* source;
	};
	std::vector<Cand> cands;
	const std::string manual = OwnCover(g);
	if (!manual.empty())
		cands.push_back({manual, "manual"});
	const std::string dir = g.path.substr(0, g.path.find_last_of('/'));
	const std::string beside = FindWithExts(dir + "/" + g.file_base);
	if (!beside.empty())
		cands.push_back({beside, "beside"});
	const std::string cache = CachePath(i);
	if (!cache.empty() && NonEmptyFile(cache))
		cands.push_back({cache, "cache"});
	bool try_download = false;
	if ((cands.empty() || (NonEmptyFile(cache) && NeedsDownload(i))) && m_allow_download)
	{
		std::lock_guard<std::mutex> lock(m_lock); // Refetch (the shelf's thread) writes m_fetched too
		try_download = !m_fetched[size_t(i)];
		m_fetched[size_t(i)] = 1;
	}
	if (try_download)
	{
		const bool counted = NeedsDownload(i);
		if (Download(i))
		{
			cands.insert(cands.begin(), {cache, "download"});
			*downloaded = true;
		}
		if (counted)
			DecrementToZero(m_to_download);
	}
	const Shots shots = LoadShots(g);
	for (const Cand& c : cands)
	{
		std::vector<uint8_t> file;
		if (!ReadFile(c.path, file))
			continue;
		GENPLUS_STAGE(Cover, "decode image");
		// libretro-thumbnails' picture: downloaded, or in the cover folder under its name (found as "manual" then)
		const bool downloaded = strcmp(c.source, "cache") == 0 || strcmp(c.source, "download") == 0 || c.path == cache;
		auto tex = Decode(file, g, CoverTurns(g, downloaded), &shots);
		if (tex)
		{
			tex->real = true;
			tex->source = c.source;
			return tex;
		}
		OrbisLog("[covers] can't decode %s", c.path.c_str());
	}
	return Placeholder(g, &shots);
}

void CoverService::Run()
{
	auto last_poll = std::chrono::steady_clock::now() - std::chrono::seconds(1);
	std::unique_lock<std::mutex> lock(m_lock);
	while (!m_quit)
	{
		const int focus = m_focus;
		int best = -1;
		for (int i = std::max(0, focus - kLoadRadius); i <= std::min(int(m_games.size()) - 1, focus + kLoadRadius); i++)
			if (m_state[size_t(i)] == 0 && (best < 0 || std::abs(i - focus) < std::abs(best - focus)))
				best = i;
		if (best >= 0)
		{
			m_state[size_t(best)] = 1;
			lock.unlock();
			static char st[48]; // one cover worker: a lasting buffer the crash handler can read
			snprintf(st, sizeof(st), "load cover %d/%zu", best, m_games.size());
			GENPLUS_STAGE(Cover, st);
			bool dl = false;
			CoverPtr tex = Load(best, &dl);
			GENPLUS_STAGE(Cover, "idle");
			lock.lock();
			if (m_state[size_t(best)] == 1)
			{
				m_ready.emplace_back(best, tex);
				m_state[size_t(best)] = 2;
				m_placeholder[size_t(best)] = tex && !tex->real ? 1 : 0;
			}
			continue;
		}
		// Everything near the selection is loaded: fetch the rest of the library to disk, nearest first.
		int fetch = -1;
		if (m_allow_download && !m_offline)
			for (size_t d = 0; d < m_games.size() && fetch < 0; d++)
				for (int sgn = -1; sgn <= 1 && fetch < 0; sgn += 2)
				{
					const long i = long(focus) + sgn * long(d);
					if (i < 0 || size_t(i) >= m_games.size() || m_fetched[size_t(i)])
						continue;
					m_fetched[size_t(i)] = 1;
					fetch = int(i);
				}
		if (fetch >= 0)
		{
			// the disk is looked at with the lock released (the shelf's thread takes it every frame)
			lock.unlock();
			if (NeedsDownload(fetch))
			{
				Download(fetch);
				DecrementToZero(m_to_download);
			}
			lock.lock();
			continue;
		}
		if (m_background)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now - last_poll >= std::chrono::milliseconds(1000))
			{
				last_poll = now;
				lock.unlock();
				PollBackground(focus);
				lock.lock();
				continue;
			}
		}
		m_wake.wait_for(lock, std::chrono::milliseconds(250));
	}
}

void CoverService::PollBackground(int focus)
{
	const int lo = std::max(0, focus - kLoadRadius), hi = std::min(int(m_games.size()) - 1, focus + kLoadRadius);
	// 1. covers the helper saved since: their placeholder cards are loaded again
	std::vector<int> cards;
	std::map<int, long long> refetching;
	{
		std::lock_guard<std::mutex> lock(m_lock);
		for (int i = lo; i <= hi; i++)
			if (m_state[size_t(i)] == 2 && m_placeholder[size_t(i)])
				cards.push_back(i);
		refetching = m_refetching;
	}
	std::vector<int> landed;
	for (int i : cards)
	{
		const std::string cache = CachePath(i);
		if (!cache.empty() && NonEmptyFile(cache))
			landed.push_back(i);
	}
	for (const auto& r : refetching)
	{
		// Square: a new file (another mtime) once the helper has it
		struct stat st = {};
		const std::string cache = CachePath(r.first);
		if (!cache.empty() && stat(cache.c_str(), &st) == 0 && st.st_size > 0 && (long long)st.st_mtime != r.second &&
			!Exists(RefetchMarker(cache)))
			landed.push_back(r.first);
	}
	if (!landed.empty())
	{
		std::lock_guard<std::mutex> lock(m_lock);
		for (int i : landed)
		{
			m_refetching.erase(i);
			if (m_state[size_t(i)] == 2)
			{
				m_state[size_t(i)] = 0;
				m_placeholder[size_t(i)] = 0;
			}
		}
	}
	// 2. the covers around the selection go first: covers/priority.txt, nearest first
	if (focus != m_prio_focus)
	{
		m_prio_focus = focus;
		std::string text;
		for (int d = 0; d <= kLoadRadius; d++)
			for (int sgn = -1; sgn <= 1; sgn += 2)
			{
				const int i = focus + sgn * d;
				if ((d == 0 && sgn > 0) || i < lo || i > hi || !NeedsDownload(i))
					continue;
				text += CoverFileFor(m_games[size_t(i)]) + "\t" + CoverUrlFor(m_games[size_t(i)]) + "\n";
			}
		if (text != m_last_prio)
		{
			m_last_prio = text;
			WriteFileAtomic(PriorityListPath(), std::vector<uint8_t>(text.begin(), text.end()));
		}
	}
	// 3. how far the helper got
	const CoverProgress p = ReadCoverProgress();
	m_bg_left = p.valid ? p.left : 0;
	m_bg_offline = p.valid && p.state == "offline";
}
} // namespace fe
