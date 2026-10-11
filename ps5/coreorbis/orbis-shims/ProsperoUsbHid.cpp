// MSX PS5 (2026-10-10, AI-assisted, from WHEN98): the helper's USB keyboard and mouse reader (ProsperoUsbHid.h).
//
// Tested on the console (13.00): Sony's ugen answers USB_GET_DEVICE_DESC, USB_GET_FULL_DESC, USB_IFACE_DRIVER_ACTIVE
// and reads of the endpoint nodes (not USB_GET_DEVICEINFO: its struct differs). A mouse's reports arrive with the
// console's driver still attached; a keyboard's don't, so a keyboard interface is detached while the app is connected.
// Each device's report descriptor (GET_DESCRIPTOR 0x22) says where the keys, buttons, X, Y and wheel are; without one
// the boot protocol's layouts are used. Sony devices and the console's own wireless module are never touched.
//
// SPDX-License-Identifier: MIT

#include "ProsperoUsbHid.h"

#include "OrbisPaths.h"

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#ifndef USB_SHORT_XFER_OK
#define USB_SHORT_XFER_OK 0x0004 // FreeBSD's dev/usb/usb_ioctl.h (ucr_flags)
#endif

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace usbhid
{
namespace
{
struct Field
{
	int report_id = 0;
	int bit = -1; // offset in the report (after the id byte)
	int size = 0;
	bool is_signed = false;
};

struct Layout
{
	bool ids = false; // the reports start with a report id
	Field buttons; // size = bits per button, count in button_count
	int button_count = 0;
	Field x, y, wheel;
	Field modifiers; // 8 one-bit fields from usage 0xe0
	Field keys; // an array of key usages
	int key_count = 0;
};

struct Device
{
	std::string ugen; // /dev/ugenB.A
	int bus = 0, addr = 0, iface = 0, ep = 0, maxpkt = 8, proto = 0;
	uint16_t vendor = 0, product = 0;
	bool detached = false;
	int fd = -1;
	Layout layout;
	bool is_keyboard = false, is_mouse = false;
	uint16_t keys[16] = {};
	int key_count = 0;
	uint8_t buttons = 0;
};

uint32_t Bits(const uint8_t* r, int len, int bit, int size)
{
	uint32_t v = 0;
	for (int i = 0; i < size && i < 32; i++)
	{
		const int b = bit + i;
		if (b / 8 < len && (r[b / 8] >> (b % 8) & 1))
			v |= 1u << i;
	}
	return v;
}

int32_t Value(const uint8_t* r, int len, const Field& f)
{
	if (f.bit < 0 || f.size <= 0)
		return 0;
	uint32_t v = Bits(r, len, f.bit, f.size);
	if (f.is_signed && f.size < 32 && (v & (1u << (f.size - 1))))
		v |= ~0u << f.size;
	return int32_t(v);
}

// A HID report descriptor's input items: where the fields this reader wants are.
Layout Parse(const uint8_t* d, int len)
{
	Layout L;
	int page = 0, rsize = 0, rcount = 0, rid = 0;
	int32_t lmin = 0;
	std::vector<uint32_t> usages;
	uint32_t umin = 0, umax = 0;
	bool have_range = false;
	int bit[256] = {};
	for (int o = 0; o < len;)
	{
		const uint8_t prefix = d[o];
		if (prefix == 0xfe) // long item
		{
			o += 3 + (o + 1 < len ? d[o + 1] : 0);
			continue;
		}
		int size = prefix & 3;
		if (size == 3)
			size = 4;
		uint32_t data = 0;
		for (int i = 0; i < size && o + 1 + i < len; i++)
			data |= uint32_t(d[o + 1 + i]) << (8 * i);
		int32_t sdata = int32_t(data);
		if (size == 1)
			sdata = int8_t(data);
		else if (size == 2)
			sdata = int16_t(data);
		const int type = (prefix >> 2) & 3, tag = prefix >> 4;
		o += 1 + size;
		if (type == 1) // global
		{
			if (tag == 0) page = int(data);
			else if (tag == 1) lmin = sdata;
			else if (tag == 7) rsize = int(data);
			else if (tag == 8) { rid = int(data) & 0xff; L.ids = true; }
			else if (tag == 9) rcount = int(data);
		}
		else if (type == 2) // local
		{
			const uint32_t u = size == 4 ? data : (uint32_t(page) << 16) | data;
			if (tag == 0) usages.push_back(u);
			else if (tag == 1) { umin = u; have_range = true; }
			else if (tag == 2) { umax = u; have_range = true; }
		}
		else if (type == 0) // main
		{
			if (tag == 8) // input
			{
				const bool constant = data & 1, variable = data & 2;
				int& at = bit[rid];
				if (!constant)
				{
					auto usage_at = [&](int i) -> uint32_t {
						if (!usages.empty())
							return usages[size_t(i) < usages.size() ? size_t(i) : usages.size() - 1];
						if (have_range)
							return umin + uint32_t(i) <= umax ? umin + uint32_t(i) : umax;
						return 0;
					};
					const uint32_t first = usage_at(0);
					if (variable && (first >> 16) == 9 && L.buttons.bit < 0)
					{
						L.buttons = {rid, at, rsize, false};
						L.button_count = rcount;
					}
					else if (variable && (first >> 16) == 7 && (first & 0xffff) == 0xe0 && L.modifiers.bit < 0)
						L.modifiers = {rid, at, 1, false};
					else if (!variable && (first >> 16) == 7 && L.keys.bit < 0)
					{
						L.keys = {rid, at, rsize, false};
						L.key_count = rcount;
					}
					else if (variable)
						for (int i = 0; i < rcount; i++)
						{
							const uint32_t u = usage_at(i);
							const Field f = {rid, at + i * rsize, rsize, lmin < 0};
							if (u == 0x10030 && L.x.bit < 0) L.x = f;
							else if (u == 0x10031 && L.y.bit < 0) L.y = f;
							else if (u == 0x10038 && L.wheel.bit < 0) L.wheel = f;
						}
				}
				at += rsize * rcount;
			}
			usages.clear();
			have_range = false;
			umin = umax = 0;
		}
	}
	return L;
}

Layout BootLayout(int proto)
{
	Layout L;
	if (proto == 1)
	{
		L.modifiers = {0, 0, 1, false};
		L.keys = {0, 16, 8, false};
		L.key_count = 6;
	}
	else
	{
		L.buttons = {0, 0, 1, false};
		L.button_count = 3;
		L.x = {0, 8, 8, true};
		L.y = {0, 16, 8, true};
		L.wheel = {0, 24, 8, true};
	}
	return L;
}

// The interface's report descriptor, by a control request on the ugen node.
int ReportDescriptor(int fd, int iface, uint8_t* out, int max)
{
	struct usb_ctl_request req;
	memset(&req, 0, sizeof(req));
	req.ucr_request.bmRequestType = 0x81; // device-to-host, standard, interface
	req.ucr_request.bRequest = 6; // GET_DESCRIPTOR
	USETW(req.ucr_request.wValue, 0x2200);
	USETW(req.ucr_request.wIndex, iface);
	USETW(req.ucr_request.wLength, max);
	req.ucr_data = out;
	req.ucr_flags = USB_SHORT_XFER_OK;
	if (ioctl(fd, USB_DO_REQUEST, &req) != 0)
		return -errno;
	return req.ucr_actlen;
}

void Scan(std::vector<Device>& devices)
{
	DIR* dir = opendir("/dev");
	if (!dir)
		return;
	while (struct dirent* e = readdir(dir))
	{
		int bus = 0, addr = 0;
		if (sscanf(e->d_name, "ugen%d.%d", &bus, &addr) != 2 || addr <= 1)
			continue;
		bool known = false;
		for (const Device& d : devices)
			known |= d.bus == bus && d.addr == addr;
		if (known)
			continue;
		const std::string path = std::string("/dev/") + e->d_name;
		const int fd = open(path.c_str(), O_RDWR);
		if (fd < 0)
			continue;
		struct usb_device_descriptor dd;
		memset(&dd, 0, sizeof(dd));
		uint8_t cfg[1024] = {};
		struct usb_gen_descriptor gd;
		memset(&gd, 0, sizeof(gd));
		gd.ugd_data = cfg;
		gd.ugd_maxlen = sizeof(cfg);
		gd.ugd_config_index = USB_UNCONFIG_INDEX;
		if (ioctl(fd, USB_GET_DEVICE_DESC, &dd) != 0 || ioctl(fd, USB_GET_FULL_DESC, &gd) != 0 ||
			UGETW(dd.idVendor) == 0x054c || dd.bDeviceClass == 0xe0)
		{
			close(fd);
			continue;
		}
		int len = cfg[2] | (cfg[3] << 8);
		if (len > int(sizeof(cfg)))
			len = sizeof(cfg);
		int iface = -1, cls = 0, alt = 0, proto = 0;
		for (int o = 0; o + 2 <= len && cfg[o] >= 2; o += cfg[o])
		{
			if (cfg[o + 1] == 4)
			{
				iface = cfg[o + 2];
				alt = cfg[o + 3];
				cls = cfg[o + 5];
				proto = cfg[o + 7];
			}
			else if (cfg[o + 1] == 5 && cls == 3 && alt == 0 && (cfg[o + 2] & 0x80) && (cfg[o + 3] & 3) == 3)
			{
				Device d;
				d.ugen = path;
				d.bus = bus;
				d.addr = addr;
				d.iface = iface;
				d.ep = cfg[o + 2] & 0x0f;
				d.maxpkt = cfg[o + 4] | (cfg[o + 5] << 8);
				d.proto = proto;
				d.vendor = UGETW(dd.idVendor);
				d.product = UGETW(dd.idProduct);
				uint8_t rd[1024];
				const int n = ReportDescriptor(fd, iface, rd, sizeof(rd));
				d.layout = n > 0 ? Parse(rd, n) : BootLayout(proto);
				const Layout& L = d.layout;
				d.is_keyboard = L.keys.bit >= 0 || L.modifiers.bit >= 0;
				d.is_mouse = L.x.bit >= 0 && L.y.bit >= 0;
				if (!d.is_keyboard && !d.is_mouse && (proto == 1 || proto == 2))
				{
					d.layout = BootLayout(proto);
					d.is_keyboard = proto == 1;
					d.is_mouse = proto == 2;
				}
				OrbisLog("[hid] %s %04x:%04x interface %d ep %d (%d bytes): report descriptor %d -> %s%s%s, ids %d, "
					"buttons %d@%d x %d@%d y %d@%d wheel %d@%d keys %dx%d@%d mods @%d",
					path.c_str(), d.vendor, d.product, iface, d.ep, d.maxpkt, n, d.is_keyboard ? "keyboard" : "",
					d.is_keyboard && d.is_mouse ? "+" : "", d.is_mouse ? "mouse" : (d.is_keyboard ? "" : "nothing used"),
					int(L.ids), L.button_count, L.buttons.bit, L.x.size, L.x.bit, L.y.size, L.y.bit, L.wheel.size,
					L.wheel.bit, L.key_count, L.keys.size, L.keys.bit, L.modifiers.bit);
				if (!d.is_keyboard && !d.is_mouse)
					continue;
				int ifc = iface;
				if (d.is_keyboard && ioctl(fd, USB_IFACE_DRIVER_ACTIVE, &ifc) == 0)
				{
					ifc = iface;
					const int r = ioctl(fd, USB_IFACE_DRIVER_DETACH, &ifc);
					d.detached = r == 0;
					OrbisLog("[hid]   the console's driver detached: %s", r == 0 ? "yes" : strerror(errno));
				}
				char ep[64];
				snprintf(ep, sizeof(ep), "/dev/usb/%d.%d.%d", bus, addr, d.ep);
				d.fd = open(ep, O_RDONLY | O_NONBLOCK);
				if (d.fd >= 0)
				{
					int one = 1;
					ioctl(d.fd, USB_SET_RX_SHORT_XFER, &one);
				}
				OrbisLog("[hid]   %s: %s", ep, d.fd >= 0 ? "reading" : strerror(errno));
				devices.push_back(d);
			}
		}
		close(fd);
	}
	closedir(dir);
}

// Hands a keyboard back to the console's driver.
void Release(Device& d)
{
	if (d.fd >= 0)
		close(d.fd);
	d.fd = -1;
	if (!d.detached)
		return;
	const int fd = open(d.ugen.c_str(), O_RDWR);
	int one = 1;
	const int r = fd >= 0 ? ioctl(fd, USB_DEVICEENUMERATE, &one) : -1;
	OrbisLog("[hid] %s handed back to the console: %s", d.ugen.c_str(), r == 0 ? "re-enumerated" : strerror(errno));
	if (fd >= 0)
		close(fd);
	d.detached = false;
}

bool SendAll(int s, const void* p, size_t n)
{
	const uint8_t* b = static_cast<const uint8_t*>(p);
	while (n)
	{
		const ssize_t w = send(s, b, n, 0);
		if (w <= 0)
			return false;
		b += w;
		n -= size_t(w);
	}
	return true;
}

// One app connection: read the devices every 2 ms until the app goes.
void Session(int client)
{
	std::vector<Device> devices;
	Scan(devices);
	Packet last;
	memset(&last, 0, sizeof(last));
	int ticks = 0, since_send = 0;
	int32_t dx = 0, dy = 0, wheel = 0;
	unsigned logged = 0;
	for (;;)
	{
		bool moved = false;
		for (size_t i = 0; i < devices.size(); i++)
		{
			Device& d = devices[i];
			uint8_t r[64];
			for (int reads = 0; reads < 8; reads++)
			{
				const ssize_t n = read(d.fd, r, d.maxpkt <= 64 ? size_t(d.maxpkt) : 64);
				if (n <= 0)
				{
					if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
					{
						OrbisLog("[hid] %s: read %s: unplugged", d.ugen.c_str(), strerror(errno));
						Release(d);
						devices.erase(devices.begin() + long(i));
						i--;
					}
					break;
				}
				if (logged < 6)
				{
					logged++;
					char hex[200];
					int o = 0;
					for (ssize_t k = 0; k < n && o < 190; k++)
						o += snprintf(hex + o, sizeof(hex) - size_t(o), " %02x", r[k]);
					OrbisLog("[hid] %s report:%s", d.ugen.c_str(), hex);
				}
				const Layout& L = d.layout;
				const uint8_t* body = r;
				int len = int(n), id = 0;
				if (L.ids)
				{
					id = r[0];
					body++;
					len--;
				}
				if (L.buttons.bit >= 0 && L.buttons.report_id == id)
					d.buttons = uint8_t(Bits(body, len, L.buttons.bit, L.button_count < 3 ? L.button_count : 3));
				if (L.x.bit >= 0 && L.x.report_id == id)
				{
					dx += Value(body, len, L.x);
					dy += Value(body, len, L.y);
					wheel += Value(body, len, L.wheel);
					moved = true;
				}
				if (L.keys.bit >= 0 && L.keys.report_id == id)
				{
					d.key_count = 0;
					if (L.modifiers.bit >= 0)
						for (int b = 0; b < 8; b++)
							if (Bits(body, len, L.modifiers.bit + b, 1))
								d.keys[d.key_count++] = uint16_t(0xe0 + b);
					for (int k = 0; k < L.key_count && d.key_count < 16; k++)
					{
						const uint32_t u = Bits(body, len, L.keys.bit + k * L.keys.size, L.keys.size);
						if (u >= 4 && u < 0xe0)
							d.keys[d.key_count++] = uint16_t(u);
					}
				}
			}
		}
		Packet p;
		memset(&p, 0, sizeof(p));
		p.magic = kMagic;
		for (const Device& d : devices)
		{
			p.keyboard |= d.is_keyboard;
			p.mouse |= d.is_mouse;
			p.buttons |= d.buttons;
			for (int k = 0; k < d.key_count && p.key_count < 24; k++)
				p.keys[p.key_count++] = d.keys[k];
		}
		const bool changed = moved || p.keyboard != last.keyboard || p.mouse != last.mouse || p.buttons != last.buttons ||
			p.key_count != last.key_count || memcmp(p.keys, last.keys, sizeof(p.keys)) != 0;
		if (changed || ++since_send >= 500)
		{
			p.dx = dx;
			p.dy = dy;
			p.wheel = wheel;
			if (!SendAll(client, &p, sizeof(p)))
				break;
			dx = dy = wheel = 0;
			last = p;
			since_send = 0;
		}
		if (++ticks >= 1000) // every 2 s: a keyboard or mouse plugged in since
		{
			ticks = 0;
			Scan(devices);
		}
		usleep(2000);
	}
	for (Device& d : devices)
		Release(d);
	OrbisLog("[hid] the app went: devices released");
}

void* Server(void*)
{
	const int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return nullptr;
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(kPort);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 1) != 0)
	{
		OrbisLog("[hid] port %d: %s (another helper serves it)", kPort, strerror(errno));
		close(s);
		return nullptr;
	}
	OrbisLog("[hid] USB keyboard and mouse served on 127.0.0.1:%d", kPort);
	for (;;)
	{
		const int c = accept(s, nullptr, nullptr);
		if (c < 0)
		{
			usleep(100000);
			continue;
		}
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); // the app gone must not kill the helper
		OrbisLog("[hid] the app connected");
		Session(c);
		close(c);
	}
	return nullptr;
}
} // namespace

void StartServer()
{
	pthread_t t;
	if (pthread_create(&t, nullptr, Server, nullptr) == 0)
		pthread_detach(t);
}
} // namespace usbhid
