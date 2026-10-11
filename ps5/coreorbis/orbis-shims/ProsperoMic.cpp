// MSX PS5: the controller's microphone - see ProsperoMic.h.
// SPDX-License-Identifier: MIT
#include "ProsperoMic.h"

#include "OrbisPaths.h"
#include "ProsperoThread.h"

#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags, void* opt, int* res);
extern "C" int sceKernelDlsym(int handle, const char* symbol, void** address);
extern "C" int sceKernelLoadStartModuleForSysmodule(const char* path, size_t args, const void* argp, uint32_t flags, void* opt,
	int* res);
// the system loads its own modules for a title that may not load them by path (0x80020063): libSceAudioIn is the internal
// system module 0x80000002 (shadPS4's sysmodule table; prospero-win loads libSceKeyboard/libSceMouse this way on the PS5)
extern "C" int sceSysmoduleLoadModuleInternal(uint32_t id);
extern "C" int sceSysmoduleGetModuleHandleInternal(uint32_t id, int32_t* handle);
extern "C" int sceSysmoduleIsLoadedInternal(uint32_t id);
extern "C" int sceKernelGetModuleInfo(int handle, void* info);
// ... so they are imports as well (stubs/audioin_stub.c): the system binds them when the app starts
extern "C" int sceAudioInOpen(int32_t user, uint32_t type, uint32_t index, uint32_t samples, uint32_t rate, uint32_t format);
extern "C" int sceAudioInInput(int32_t handle, void* dest);
extern "C" int sceAudioInClose(int32_t handle);
// why a port hears nothing: 1 no device, 2 low priority, 8 unusable format (0 = it can hear; shadPS4's audioin.h)
extern "C" int sceAudioInGetSilentState(int32_t handle);
// the user's microphone mute (the light); arguments as the PS4's (user, 1 = muted) - its return is logged
extern "C" int sceAudioInSetUserMute(int32_t user, int32_t mute);
extern "C" int sceKernelGetModuleList(int* handles, size_t count, size_t* actual);

namespace ps5mic
{
namespace
{
// libSceAudioIn (as the PS4's): open(user, type, index, samples a read, rate, format) -> handle; input blocks until
// `samples` samples are in; close.
using OpenFn = int (*)(int32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using InputFn = int (*)(int32_t, void*);
using CloseFn = int (*)(int32_t);
OpenFn s_open = nullptr;
InputFn s_input = nullptr;
CloseFn s_close = nullptr;

std::mutex s_lock;
ps5::BigThread s_thread;
std::atomic<bool> s_run{false};
std::atomic<int> s_handle{-1};
std::atomic<bool> s_live{false};
std::atomic<float> s_blow{0.0f};
std::atomic<bool> s_unmuted{false};

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

bool Load()
{
	if (s_open && s_input && s_close)
		return true;
	static int module = -1;
	if (module < 0)
	{
		constexpr uint32_t kAudioIn = 0x80000002;
		const int loaded = sceSysmoduleIsLoadedInternal(kAudioIn);
		const int load = loaded == 0 ? 0 : sceSysmoduleLoadModuleInternal(kAudioIn);
		int32_t handle = -1;
		const int got = sceSysmoduleGetModuleHandleInternal(kAudioIn, &handle);
		OrbisLog("[mic] system module libSceAudioIn: loaded %x, load %x, handle %x (%d)", unsigned(loaded), unsigned(load),
			unsigned(got), int(handle));
		if (got == 0 && handle >= 0)
			module = handle;
	}
	if (module < 0)
	{
		const char* path = "/system/common/lib/libSceAudioIn.sprx";
		module = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
		OrbisLog("[mic] load libSceAudioIn -> %x", unsigned(module));
		if (module < 0)
		{
			// (80020063: the SDK version check - the system's own loader may not make it)
			module = sceKernelLoadStartModuleForSysmodule(path, 0, nullptr, 0, nullptr, nullptr);
			OrbisLog("[mic] load libSceAudioIn (for sysmodule) -> %x", unsigned(module));
		}
		if (module < 0)
			return false;
	}
	// which module the handle is, and what it answers for each name (attempt 2 found the module loaded, handle 109, but
	// no sceAudioInOpen/Input/Close in it): the other open calls, and the other loaded modules
	{
		alignas(8) unsigned char info[0x400] = {};
		*reinterpret_cast<uint64_t*>(info) = 0x160;
		const int r = sceKernelGetModuleInfo(module, info);
		OrbisLog("[mic] module %d info -> %x name \"%.64s\"", module, unsigned(r), reinterpret_cast<const char*>(info + 8));
	}
	static const char* const kNames[] = {"sceAudioInOpen", "sceAudioInInput", "sceAudioInClose", "sceAudioInOpenEx",
		"sceAudioInHqOpen", "sceAudioInHqOpenEx", "sceAudioInExtOpen", "sceAudioInExtInput", "sceAudioInInputs",
		"sceAudioInDeviceOpen", "sceAudioInGetSilentState", "sceAudioInInit", "sceAudioOutOpen"};
	for (const char* name : kNames)
	{
		void* p = nullptr;
		const int r = sceKernelDlsym(module, name, &p);
		OrbisLog("[mic] dlsym(%d, %s) -> %x %p", module, name, unsigned(r), p);
	}
	{
		int handles[256];
		size_t count = 0;
		const int r = sceKernelGetModuleList(handles, 256, &count);
		OrbisLog("[mic] module list -> %x, %d modules", unsigned(r), int(count));
		for (size_t i = 0; i < count && i < 256; i++)
		{
			void* p = nullptr;
			if (sceKernelDlsym(handles[i], "sceAudioInOpen", &p) == 0 && p)
			{
				OrbisLog("[mic] sceAudioInOpen found in module %d", handles[i]);
				if (!s_open)
					module = handles[i];
			}
		}
	}
	sceKernelDlsym(module, "sceAudioInOpen", reinterpret_cast<void**>(&s_open));
	sceKernelDlsym(module, "sceAudioInInput", reinterpret_cast<void**>(&s_input));
	sceKernelDlsym(module, "sceAudioInClose", reinterpret_cast<void**>(&s_close));
	if (!s_open || !s_input || !s_close)
	{
		// sceKernelDlsym finds nothing in the system's libSceAudioIn: the imports the app was linked with
		s_open = sceAudioInOpen, s_input = sceAudioInInput, s_close = sceAudioInClose;
		OrbisLog("[mic] using the linked imports");
	}
	OrbisLog("[mic] sceAudioInOpen %p, Input %p, Close %p", reinterpret_cast<void*>(s_open), reinterpret_cast<void*>(s_input),
		reinterpret_cast<void*>(s_close));
	return s_open && s_input && s_close;
}

void Listen(int handle, unsigned samples, unsigned rate)
{
	int16_t* buf = static_cast<int16_t*>(calloc(samples * 2, sizeof(int16_t))); // (room for stereo)
	if (!buf)
		return;
	double last_sound = -10, last_log = Now(), loud_since = -1;
	float lp = 0, blow = 0, peak_rms = 0, peak_low = 0, peak_blow = 0;
	int errors = 0, reads = 0, blow_blocks = 0;
	const float block_s = float(samples) / float(rate);
	while (s_run)
	{
		const int r = s_input(handle, buf);
		if (!s_run)
			break;
		if (r < 0)
		{
			if (errors++ < 5)
				OrbisLog("[mic] sceAudioInInput -> %x", unsigned(r));
			s_live = false;
			usleep(50000);
			continue;
		}
		reads++;
		s_unmuted = !(sceAudioInGetSilentState(handle) & 4); // 4: muted by the user (the light)
		// loudness, and how much of it is low rumble (blowing is; speech much less)
		double sum = 0, low = 0;
		bool sound = false;
		for (unsigned i = 0; i < samples; i++)
		{
			const float x = buf[i] / 32768.0f;
			sound |= buf[i] != 0;
			lp += 0.06f * (x - lp); // ~150 Hz at 16 kHz
			sum += double(x) * x;
			low += double(lp) * lp;
		}
		const double now = Now();
		if (sound)
			last_sound = now;
		s_live = now - last_sound < 0.4; // muted: exact silence
		const float rms = float(std::sqrt(sum / samples)), low_ratio = sum > 0 ? float(low / sum) : 0.0f;
		peak_rms = std::max(peak_rms, rms), peak_low = std::max(peak_low, low_ratio);
		// a blow: loud and rumbling for a moment (a word or a tap is too short or not low enough). Measured on the
		// DualSense (2026-10-11): blowing peaks at rms ~0.53 with a low part of 0.27-0.52, quiet is ~0.003 -> the old
		// 0.08 / 0.30 bar missed blows; anything very loud counts too
		const bool loud = (rms > 0.06f && low_ratio > 0.15f) || rms > 0.20f;
		if (loud && loud_since < 0)
			loud_since = now;
		if (!loud)
			loud_since = -1;
		const float target = (loud && now - loud_since > 0.05) ? std::min(1.0f, (rms - 0.04f) / 0.20f) : 0.0f;
		blow += (target - blow) * (target > blow ? 0.6f : std::min(1.0f, block_s * 6.0f));
		s_blow = blow;
		if (target > 0)
			blow_blocks++;
		peak_blow = std::max(peak_blow, blow);
		if (now - last_log > 5)
		{
			OrbisLog("[mic] %d reads, live %d, loudest %.3f (low part %.2f), silent state %x, blowing %d blocks (strongest %.2f)",
				reads, int(s_live.load()), peak_rms, peak_low, unsigned(sceAudioInGetSilentState(handle)), blow_blocks, peak_blow);
			last_log = now, peak_rms = 0, peak_low = 0, reads = 0, blow_blocks = 0, peak_blow = 0;
		}
	}
	free(buf);
}
} // namespace

bool Start(int32_t user)
{
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_run)
		return true;
	if (!Load())
		return false;
	// the type / rate pairs to try, the first one opened is used
	struct Try
	{
		uint32_t type, samples, rate;
	};
	const Try tries[] = {{1, 256, 16000}, {0, 256, 16000}, {5, 256, 16000}, {1, 256, 48000}};
	int handle = -1;
	Try used = {};
	// attempt 3 opened type 0 but read only exact zeros: try each type for a moment and keep the first that hears
	// something (else the first that opened), logging the library's own reason for silence
	int16_t probe[48000 / 100 * 2 * 2] = {};
	int first = -1;
	Try first_try = {};
	for (const Try& t : tries)
	{
		const int h = s_open(user, t.type, 0, t.samples, t.rate, 0);
		const int silent = h >= 0 ? sceAudioInGetSilentState(h) : -1;
		OrbisLog("[mic] sceAudioInOpen(user %x, type %u, %u samples, %u Hz) -> %x, silent state %x", unsigned(user), t.type,
			t.samples, t.rate, unsigned(h), unsigned(silent));
		if (h < 0)
			continue;
		bool heard = false;
		for (int i = 0; i < 25 && !heard; i++)
		{
			if (s_input(h, probe) < 0)
				break;
			for (unsigned k = 0; k < t.samples && !heard; k++)
				heard = probe[k] != 0;
		}
		OrbisLog("[mic]   type %u: %s", t.type, heard ? "hears sound" : "exact silence");
		if (heard)
		{
			if (first >= 0)
				s_close(first);
			handle = h, used = t;
			break;
		}
		if (first < 0 && !(silent & 2))
			first = h, first_try = t;
		else
			s_close(h);
	}
	if (handle < 0 && first >= 0)
		handle = first, used = first_try;
	if (handle < 0)
		return false;
	// the shelf starts with the microphone muted (user request): games in the normal view until the button is pressed
	const int muted = sceAudioInSetUserMute(user, 1);
	s_unmuted = !(sceAudioInGetSilentState(handle) & 4);
	OrbisLog("[mic] sceAudioInSetUserMute(%x, 1) -> %x; now %s", unsigned(user), unsigned(muted), s_unmuted ? "unmuted" : "muted");
	s_handle = handle;
	s_run = true;
	s_thread = ps5::BigThread([handle, used] { Listen(handle, used.samples, used.rate); }, 256 * 1024);
	return true;
}

void Stop()
{
	std::lock_guard<std::mutex> lock(s_lock);
	if (!s_run)
		return;
	s_run = false;
	const int h = s_handle.exchange(-1);
	if (h >= 0)
		s_close(h); // (wakes the read)
	s_thread.join();
	s_live = false;
	s_blow = 0;
	s_unmuted = false;
}

bool Live()
{
	return s_live;
}

bool Available()
{
	return s_run;
}

bool Unmuted()
{
	return s_unmuted;
}

float Blow()
{
	return s_blow;
}
} // namespace ps5mic
