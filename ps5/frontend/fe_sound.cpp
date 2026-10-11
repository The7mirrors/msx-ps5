// MSX PS5 (2026-10-11, AI-assisted): the shelf's sound - see fe_sound.h. The music is fe_music's.
// SPDX-License-Identifier: MIT
#include "fe_sound.h"

#include "OrbisPaths.h"
#include "ProsperoAudio.h"
#include "ProsperoThread.h"
#include "fe_music.h"
#include "fe_settings.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>

namespace fe
{
namespace sound
{
namespace
{
constexpr int kRate = 48000;
constexpr float kMusicLevel = 0.6f; // the music under the effects at 100% (Settings > Audio > Shelf music volume)

const char* const kFiles[int(Sfx::Count)] = {"browse", "menu_move", "menu_ok", "menu_back", "open", "close", "favorite",
	"cart_clack", "floppy_insert", "floppy_seek", "shutter_slide", "shutter_snap"};
std::vector<int16_t> g_sfx[int(Sfx::Count)]; // 48 kHz stereo
bool g_loaded = false;

struct Voice
{
	const std::vector<int16_t>* data = nullptr;
	size_t pos = 0; // in frames
	float volume = 1;
};
Voice g_voices[8];
float g_music = 0, g_music_target = 0; // the music's level, fading
bool g_music_wanted = false;           // the shelf wants music (Music(true))
int g_track = -1;                      // the track playing (0-based), -1 none
// the mixer's own thread keeps the ring filled while the shelf is up (its drawing can take a while: a style change
// rebuilds every picture); with a game loaded the game feeds the ring, and its menus call Pump
std::mutex g_lock;
std::atomic<bool> g_active{false};
ps5::BigThread g_thread;
void Fill(); // (below)

bool LoadWav(const std::string& path, std::vector<int16_t>& out)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::vector<uint8_t> b;
	uint8_t chunk[65536];
	size_t got;
	while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0)
		b.insert(b.end(), chunk, chunk + got);
	fclose(f);
	if (b.size() < 44 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4))
		return false;
	size_t p = 12;
	while (p + 8 <= b.size())
	{
		uint32_t len;
		memcpy(&len, b.data() + p + 4, 4);
		if (!memcmp(b.data() + p, "data", 4))
		{
			len = uint32_t(std::min<size_t>(len, b.size() - p - 8));
			out.resize(len / 2);
			memcpy(out.data(), b.data() + p + 8, out.size() * 2);
			return true;
		}
		p += 8 + len + (len & 1);
	}
	return false;
}

// The track the settings ask for (1-based in the settings, 0 = off) playing, or fading out.
void FollowSettings()
{
	const int want = Config().shelf_music - 1;
	if (want >= 0 && want != g_track)
	{
		music::Select(want); // (from its start; the change is heard at once in Settings > Audio)
		g_track = want;
		OrbisLog("[sound] music: %s", music::TrackName(want));
	}
	g_music_target = g_music_wanted && want >= 0 ? 1.0f : 0.0f;
}
} // namespace

void Init()
{
	if (g_loaded)
		return;
	g_loaded = true;
	int ok = 0;
	for (int i = 0; i < int(Sfx::Count); i++)
		ok += LoadWav(OrbisRoot() + "/sounds/" + kFiles[i] + ".wav", g_sfx[i]) ? 1 : 0;
	OrbisLog("[sound] %d of %d effects loaded from %s/sounds", ok, int(Sfx::Count), OrbisRoot().c_str());
	g_thread = ps5::BigThread([] {
		for (;;)
		{
			if (g_active)
			{
				std::lock_guard<std::mutex> lock(g_lock);
				if (g_active)
					Fill();
			}
			usleep(4000);
		}
	}, 256 * 1024);
}

void Play(Sfx s, float volume)
{
	if (!Config().shelf_sounds || int(s) < 0 || int(s) >= int(Sfx::Count) || g_sfx[int(s)].empty())
		return;
	std::lock_guard<std::mutex> lock(g_lock);
	Voice* v = nullptr;
	for (Voice& c : g_voices)
		if (!c.data)
		{
			v = &c;
			break;
		}
	if (!v) // all busy: the one furthest along
	{
		v = &g_voices[0];
		for (Voice& c : g_voices)
			if (c.pos > v->pos)
				v = &c;
	}
	v->data = &g_sfx[int(s)], v->pos = 0, v->volume = volume;
}

void Music(bool on)
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_music_wanted = on;
	if (on)
		g_active = true;
	FollowSettings();
}

void StopAll()
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_active = false; // the game feeds the ring now
	for (Voice& v : g_voices)
		v.data = nullptr;
	g_music = g_music_target = 0;
	g_music_wanted = false;
}

namespace
{
// Tops the ring up (g_lock held).
void Fill()
{
	if (!ps5audio::Available())
		return;
	FollowSettings();
	constexpr int kTarget = kRate * 70 / 1000; // 70 ms queued
	const int need = std::min(kTarget - ps5audio::Queued(), 4096);
	if (need <= 0)
		return;
	static std::vector<float> ml, mr;
	static std::vector<int16_t> out;
	ml.assign(size_t(need), 0.0f), mr.assign(size_t(need), 0.0f);
	out.resize(size_t(need) * 2);
	if (g_music > 0 || g_music_target > 0)
		music::Render(ml.data(), mr.data(), need);
	const float fade_step = 1.0f / (kRate * 0.8f);
	const float vol = std::max(0, std::min(100, Config().volume)) / 100.0f;
	for (int i = 0; i < need; i++)
	{
		g_music += g_music < g_music_target ? std::min(fade_step, g_music_target - g_music)
			: -std::min(fade_step, g_music - g_music_target);
		const float k = kMusicLevel * g_music * std::max(0, std::min(100, Config().shelf_music_volume)) / 100.0f;
		float l = ml[size_t(i)] * k, r = mr[size_t(i)] * k;
		for (Voice& v : g_voices)
		{
			if (!v.data)
				continue;
			if (v.pos * 2 + 1 >= v.data->size())
			{
				v.data = nullptr;
				continue;
			}
			l += (*v.data)[v.pos * 2] / 32768.0f * v.volume;
			r += (*v.data)[v.pos * 2 + 1] / 32768.0f * v.volume;
			v.pos++;
		}
		out[size_t(i) * 2] = int16_t(std::clamp(l * vol, -1.0f, 1.0f) * 32767);
		out[size_t(i) * 2 + 1] = int16_t(std::clamp(r * vol, -1.0f, 1.0f) * 32767);
	}
	ps5audio::Push(out.data(), need);
}
} // namespace

void Pump()
{
	if (g_active)
		return; // (the thread does it)
	std::lock_guard<std::mutex> lock(g_lock);
	Fill();
}
} // namespace sound
} // namespace fe
