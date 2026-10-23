// MSX PS5 frontend: HTTPS GET (fe_http.h).
//
// 2026-10-10 (AI-assisted): Mbed TLS 3.6 (PS5SX2's TLS 1.2 build, third_party/mbedtls/port) over a plain socket,
// with Mozilla's CA bundle built in (third_party/cacert.c), instead of libSceHttp2: in the helper payload every
// libSceHttp2 request failed at once (send 8095f00c). HTTP/1.0 (no chunked bodies), redirects followed (3 at
// most), 10 s per socket operation, every request logged with its result.
//
// SPDX-License-Identifier: MIT

#include "fe_http.h"

#include "OrbisPaths.h"
#include "ProsperoCrash.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#if !defined(__ANDROID__)
#include <sys/sysctl.h>
#else
#include <stdlib.h> // arc4random_buf
#endif
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

extern "C" const unsigned char dw_cacert[];
extern "C" const size_t dw_cacert_len;

namespace fe
{
namespace
{
double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// one TLS configuration for the process (the roots parsed once)
struct Tls
{
	std::mutex lock;
	bool tried = false, ok = false;
	mbedtls_x509_crt roots;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_ssl_config conf;
};
Tls& T()
{
	static Tls t;
	return t;
}

int Entropy(void*, unsigned char* out, size_t len, size_t* olen)
{
#if defined(__ANDROID__)
	arc4random_buf(out, len); // (bionic: the kernel's random source)
	*olen = len;
	return 0;
#else
	size_t done = 0;
	while (done < len)
	{
		size_t chunk = std::min<size_t>(len - done, 256), got = chunk;
		int mib[2] = {CTL_KERN, KERN_ARND};
		if (sysctl(mib, 2, out + done, &got, nullptr, 0) != 0 || got == 0 || got > chunk)
			return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
		done += got;
	}
	*olen = len;
	return 0;
#endif
}

bool TlsInit()
{
	Tls& t = T();
	std::lock_guard<std::mutex> g(t.lock);
	if (t.tried)
		return t.ok;
	t.tried = true;
	mbedtls_x509_crt_init(&t.roots);
	mbedtls_entropy_init(&t.entropy);
	mbedtls_ctr_drbg_init(&t.drbg);
	mbedtls_ssl_config_init(&t.conf);
	int r = mbedtls_x509_crt_parse(&t.roots, dw_cacert, dw_cacert_len);
	if (r < 0)
	{
		OrbisLog("[http] CA bundle not parsed (-0x%x)", unsigned(-r));
		return false;
	}
	if ((r = mbedtls_entropy_add_source(&t.entropy, Entropy, nullptr, 32, MBEDTLS_ENTROPY_SOURCE_STRONG)) != 0 ||
		(r = mbedtls_ctr_drbg_seed(&t.drbg, mbedtls_entropy_func, &t.entropy, reinterpret_cast<const unsigned char*>("msx-ps5"), 7)) != 0 ||
		(r = mbedtls_ssl_config_defaults(&t.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
	{
		OrbisLog("[http] TLS setup failed (-0x%x)", unsigned(-r));
		return false;
	}
	mbedtls_ssl_conf_authmode(&t.conf, MBEDTLS_SSL_VERIFY_REQUIRED); // a server the roots don't vouch for is refused
	mbedtls_ssl_conf_ca_chain(&t.conf, &t.roots, nullptr);
	mbedtls_ssl_conf_rng(&t.conf, mbedtls_ctr_drbg_random, &t.drbg);
	t.ok = true;
	OrbisLog("[http] TLS ready (Mbed TLS, built-in CA bundle)");
	return true;
}

int BioSend(void* ctx, const unsigned char* buf, size_t len)
{
	const ssize_t r = send(*static_cast<int*>(ctx), buf, len, 0);
	return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : int(r);
}

int BioRecv(void* ctx, unsigned char* buf, size_t len)
{
	const ssize_t r = recv(*static_cast<int*>(ctx), buf, len, 0);
	if (r < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	return int(r);
}

int Connect(const std::string& host, int port, double timeout_s)
{
	addrinfo hints = {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* res = nullptr;
	char ports[8];
	snprintf(ports, sizeof(ports), "%d", port);
	if (getaddrinfo(host.c_str(), ports, &hints, &res) != 0 || !res)
		return -1;
	int fd = -1;
	for (addrinfo* a = res; a && fd < 0; a = a->ai_next)
	{
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0)
			continue;
		timeval tv;
		tv.tv_sec = long(timeout_s);
		tv.tv_usec = long((timeout_s - double(tv.tv_sec)) * 1e6);
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		if (connect(fd, a->ai_addr, a->ai_addrlen) != 0)
		{
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(res);
	return fd;
}

bool SplitUrl(const std::string& url, std::string* host, std::string* path)
{
	if (url.compare(0, 8, "https://") != 0)
		return false;
	const size_t slash = url.find('/', 8);
	*host = url.substr(8, slash == std::string::npos ? std::string::npos : slash - 8);
	*path = slash == std::string::npos ? "/" : url.substr(slash);
	return !host->empty() && host->find(':') == std::string::npos;
}
} // namespace

std::string UrlEncode(const std::string& s)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s)
	{
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
			c == '.' || c == '~')
			out += char(c);
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

bool Http::Init()
{
	if (m_tried)
		return m_ok;
	m_tried = true;
	GENPLUS_STAGE(Cover, "http: tls init");
	m_ok = TlsInit();
	return m_ok;
}

void Http::Abort()
{
	m_stopping.store(true);
	std::lock_guard<std::mutex> lock(m_abort);
	const int fd = m_active.load();
	if (fd >= 0)
	{
		shutdown(fd, SHUT_RDWR); // the blocked send/recv returns at once
		OrbisLog("[http] abort: socket %d shut down", fd);
	}
}

void Http::Term()
{
	m_tried = m_ok = false;
	m_stopping.store(false); // Abort() was for the downloads of that run: the next Start downloads again
}

int Http::Get(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes)
{
	// A transport failure is tried again after a pause: the console's network can still be settling when the
	// shelf opens.
	int status = -1;
	for (int attempt = 1; attempt <= 3 && !m_stopping && Remaining() > 1.0; attempt++)
	{
		status = GetOnce(url, out, max_bytes, attempt);
		if (status != -1)
			break;
		for (int i = 0; i < 15 && !m_stopping && Remaining() > 1.0; i++)
			usleep(100 * 1000);
	}
	return status;
}

double Http::Remaining() const
{
	return m_deadline > 0 ? m_deadline - Now() : 1e9;
}

int Http::GetOnce(const std::string& first_url, std::vector<uint8_t>& out, size_t max_bytes, int attempt)
{
	out.clear();
	if (m_stopping || !Init())
		return -1;
	const double t0 = Now();
	std::string url = first_url, note;
	int status = -1;
	for (int hop = 0; hop < 4 && !m_stopping; hop++)
	{
		std::string host, path;
		if (!SplitUrl(url, &host, &path))
		{
			note = "not an https:// address";
			break;
		}
		const double timeout = std::min(10.0, std::max(1.0, Remaining()));
		const int fd = Connect(host, 443, timeout);
		if (fd < 0)
		{
			note = "no connection to " + host;
			break;
		}
		{
			std::lock_guard<std::mutex> lock(m_abort);
			m_active = fd;
		}
		int sock = fd;
		mbedtls_ssl_context ssl;
		mbedtls_ssl_init(&ssl);
		int r = mbedtls_ssl_setup(&ssl, &T().conf);
		if (r == 0)
			r = mbedtls_ssl_set_hostname(&ssl, host.c_str()); // the name the certificate must carry, and SNI
		if (r == 0)
		{
			mbedtls_ssl_set_bio(&ssl, &sock, BioSend, BioRecv, nullptr);
			while ((r = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			{
			}
		}
		std::string location;
		status = -1;
		if (r == 0)
		{
			const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: MSXPS5/" GENPLUS_PS5_VERSION
				"\r\nAccept: */*\r\nConnection: close\r\n\r\n";
			for (size_t sent = 0; sent < req.size() && r >= 0;)
			{
				r = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(req.data()) + sent, req.size() - sent);
				if (r > 0)
					sent += size_t(r);
				else if (r == MBEDTLS_ERR_SSL_WANT_WRITE)
					r = 0;
			}
			std::string head;
			bool in_body = false;
			unsigned char buf[16384];
			while (r >= 0)
			{
				r = mbedtls_ssl_read(&ssl, buf, sizeof(buf));
				if (r == MBEDTLS_ERR_SSL_WANT_READ)
				{
					r = 0;
					continue;
				}
				if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
				{
					r = 0;
					break;
				}
				if (r < 0)
					break;
				if (in_body)
				{
					out.insert(out.end(), buf, buf + r);
				}
				else
				{
					head.append(reinterpret_cast<const char*>(buf), size_t(r));
					const size_t end = head.find("\r\n\r\n");
					if (end == std::string::npos)
					{
						if (head.size() > 65536)
						{
							r = -1;
							break;
						}
						continue;
					}
					in_body = true;
					sscanf(head.c_str(), "HTTP/1.%*d %d", &status);
					out.assign(head.begin() + long(end + 4), head.end());
					// a redirect's target
					std::string lower = head.substr(0, end);
					for (char& c : lower)
						c = char(tolower(static_cast<unsigned char>(c)));
					const size_t loc = lower.find("\r\nlocation:");
					if (loc != std::string::npos)
					{
						size_t a = loc + 11;
						while (a < end && head[a] == ' ')
							a++;
						const size_t b = head.find("\r\n", a);
						location = head.substr(a, b - a);
					}
				}
				if (out.size() > max_bytes)
				{
					status = -2;
					break;
				}
			}
			if (r < 0 && status == 200)
				status = -1; // cut off before the end
		}
		if (r < 0)
		{
			char e[96];
			mbedtls_strerror(r, e, sizeof(e));
			note = host + ": " + e;
		}
		mbedtls_ssl_close_notify(&ssl);
		mbedtls_ssl_free(&ssl);
		{
			std::lock_guard<std::mutex> lock(m_abort);
			m_active = -1;
		}
		close(fd);
		if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && !location.empty())
		{
			url = location[0] == '/' ? "https://" + host + location : location;
			out.clear();
			continue;
		}
		break;
	}
	OrbisLog("[http] try %d: GET %s -> %d, %zu bytes, %.0f ms%s%s", attempt, first_url.c_str(), status, out.size(),
		(Now() - t0) * 1000.0, note.empty() ? "" : ", ", note.c_str());
	if (status != 200)
		out.clear();
	return status;
}
} // namespace fe
