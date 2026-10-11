// MSX PS5 frontend: HTTPS GET with Mbed TLS over plain sockets (PS5SX2's way since its covers patch, 2026-10-09):
// the console's libSceHttp2 fails every request once the process is out of its sandbox (send 8095f00c).
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace fe
{
class Http
{
public:
	~Http() { Term(); }
	// The HTTP status (200 on success), or -1 when the request could not be made (no network...).
	// Only one thread may call Get.
	int Get(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes = 8u << 20);

private:
	int GetOnce(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes, int attempt);

public:
	// Any thread: fails the request in flight (shutdown).
	void Abort();
	void Term();
	bool Offline() const { return m_tried && !m_ok; }
	// No request (or retry) runs past this CLOCK_MONOTONIC time in seconds, and the timeouts of the last one
	// are cut to fit it; 0: no limit (the shelf's downloads).
	void SetDeadline(double t) { m_deadline = t; }

private:
	bool Init();
	double Remaining() const; // seconds left before the deadline (a large number when there is none)
	double m_deadline = 0;
	bool m_tried = false, m_ok = false;
	std::atomic<int> m_active{-1}; // the socket of the request in flight
	std::atomic<bool> m_stopping{false};
	std::mutex m_abort;
};

// "Super Metroid (Japan, USA) (En,Ja)" -> "Super%20Metroid%20%28Japan%2C%20USA%29%20%28En%2CJa%29"
std::string UrlEncode(const std::string& s);
} // namespace fe
