// MSX PS5 (2026-10-10, AI-assisted, from WHEN98): a real USB keyboard and mouse (ProsperoKbdMouse.h).
//
// The console's libSceKeyboard and libSceMouse are out of reach on firmware 12+ (the app may not load them:
// 0x80020063; in a payload they load but their init never returns). The helper payload reads the devices from USB
// itself (ProsperoUsbHid.cpp) and sends their state to 127.0.0.1:9089; this thread receives it, reconnecting every
// 2 s while the helper isn't there (an older helper, or none yet).
//
// SPDX-License-Identifier: MIT

#include "ProsperoKbdMouse.h"

#include "OrbisPaths.h"
#include "ProsperoUsbHid.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <mutex>

namespace ps5kbm
{
namespace
{
std::mutex g_lock;
Snapshot g_state; // under g_lock

bool RecvAll(int s, void* p, size_t n)
{
	uint8_t* b = static_cast<uint8_t*>(p);
	while (n)
	{
		const ssize_t r = recv(s, b, n, 0);
		if (r <= 0)
			return false;
		b += r;
		n -= size_t(r);
	}
	return true;
}

void* Thread(void*)
{
	bool logged_wait = false;
	for (;;)
	{
		const int s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0)
		{
			sleep(2);
			continue;
		}
		sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_port = htons(usbhid::kPort);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
		{
			if (!logged_wait)
				OrbisLog("[kbm] no USB keyboard/mouse service on port %d yet (an older helper?): retrying", usbhid::kPort);
			logged_wait = true;
			close(s);
			sleep(2);
			continue;
		}
		int one = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		OrbisLog("[kbm] connected to the helper's USB keyboard/mouse service");
		bool kseen = false, mseen = false;
		usbhid::Packet p;
		while (RecvAll(s, &p, sizeof(p)) && p.magic == usbhid::kMagic)
		{
			if (p.keyboard && !kseen)
				OrbisLog("[kbm] keyboard connected");
			if (p.mouse && !mseen)
				OrbisLog("[kbm] mouse connected");
			kseen |= p.keyboard != 0;
			mseen |= p.mouse != 0;
			std::lock_guard<std::mutex> g(g_lock);
			g_state.keyboard = p.keyboard != 0;
			g_state.mouse = p.mouse != 0;
			g_state.key_count = p.key_count <= 24 ? p.key_count : 24;
			memcpy(g_state.keys, p.keys, sizeof(g_state.keys));
			g_state.buttons = p.buttons;
			g_state.dx += p.dx;
			g_state.dy += p.dy;
			g_state.wheel += p.wheel;
		}
		OrbisLog("[kbm] the helper's service closed: reconnecting");
		close(s);
		{
			std::lock_guard<std::mutex> g(g_lock);
			g_state = Snapshot();
		}
		sleep(2);
	}
	return nullptr;
}
} // namespace

void Start()
{
	static bool started = false;
	if (started)
		return;
	started = true;
	pthread_t t;
	if (pthread_create(&t, nullptr, Thread, nullptr) == 0)
		pthread_detach(t);
}

Snapshot Take()
{
	std::lock_guard<std::mutex> g(g_lock);
	Snapshot s = g_state;
	g_state.dx = g_state.dy = g_state.wheel = 0;
	return s;
}
} // namespace ps5kbm
