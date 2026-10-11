// The 3D models (fe_cart3d) as wireframes, beside the finished look: cart3d_wire cover.png out.ppm
#include "../frontend/fe_cart3d.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../frontend/third_party/stb_image.h"

#include <cstdio>
#include <vector>

using namespace cart3d;

int main(int argc, char** argv)
{
	if (argc < 3)
		return 1;
	int w, h, c;
	unsigned char* d = stbi_load(argv[1], &w, &h, &c, 4);
	Art a;
	a.front = Texture::FromPixels(reinterpret_cast<uint32_t*>(d), w, h);
	a.back = a.spine = a.label = a.front;
	a.konami = true;
	View v;
	std::vector<uint32_t> f(size_t(v.w) * v.h);
	for (int y = 0; y < v.h; y++)
		for (int x = 0; x < v.w; x++)
			f[size_t(y) * v.w + x] = y < 600 ? 0xff1a1414u : 0xff100c0cu;
	struct Item
	{
		Kind k;
		float x, height, yaw;
	};
	const Item items[3] = {{Kind::Box, -640, 380, 0.55f}, {Kind::Cartridge, -20, 215, 0.5f}, {Kind::Floppy, 650, 290, 0.5f}};
	for (const Item& it : items)
	{
		// left half of each: the finished object; right half: its wireframe (hidden edges dimmer)
		Pose p;
		p.x = it.x - 165, p.z = 1150, p.height = it.height, p.yaw = it.yaw, p.pitch = 0.15f;
		Draw(f.data(), v, it.k, p, a, 0, v.w);
		Pose q = p;
		q.x = it.x + 175;
		const int tris = Wireframe(f.data(), v, it.k, q, 0xffffe060u, 0xff6a4a30u);
		std::printf("%s: %d triangles\n", it.k == Kind::Box ? "box" : it.k == Kind::Cartridge ? "cartridge" : "floppy", tris);
	}
	FILE* o = std::fopen(argv[2], "wb");
	std::fprintf(o, "P6\n%d %d\n255\n", v.w, v.h);
	for (uint32_t px : f)
	{
		const unsigned char rgb[3] = {uint8_t(px), uint8_t(px >> 8), uint8_t(px >> 16)};
		std::fwrite(rgb, 1, 3, o);
	}
	std::fclose(o);
}
