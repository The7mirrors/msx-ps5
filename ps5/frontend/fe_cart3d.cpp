// MSX PS5 frontend: the 3D game objects (fe_cart3d.h).
//
// SPDX-License-Identifier: MIT
#include "fe_cart3d.h"

#include "msx_logo.inc"
#include "alamiah_logo.inc"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cart3d
{
namespace
{
struct V3
{
	float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 Mul(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 Norm(V3 a)
{
	const float l = std::sqrt(Dot(a, a));
	return l > 1e-12f ? a * (1.0f / l) : V3{0, 0, -1};
}

enum Face : uint8_t
{
	FFront, FBack, FLeft, FRight, FTop, FBottom,
	FCavity, // inside the cartridge's connector opening
	FBoard   // the board in it
};

struct Vert
{
	V3 p, n; // object space: base centre at the origin, height 1, front face towards -z
	float u, v;
};
struct Tri
{
	uint16_t a, b, c;
	uint8_t face;
};
struct Mesh
{
	std::vector<Vert> v;
	std::vector<Tri> t;
	float w = 1, h = 1, d = 1; // size (h = 1)
};

// A box with rounded edges: each face a grid that is dense near its edges, every point pushed onto the rounded
// surface (the classic "rounded box": clamp into the inner box, then out by the radius).
Mesh RoundedBox(float w, float d, float r, int seg)
{
	Mesh m;
	m.w = w, m.h = 1, m.d = d;
	const V3 half{w / 2, 0.5f, d / 2}, centre{0, 0.5f, 0};
	auto coords = [&](float ha) {
		std::vector<float> c;
		for (int k = 0; k <= seg; k++)
		{
			const float a = float(k) / seg * 1.5707963f;
			c.push_back(-ha + r * (1 - std::cos(a)));
		}
		for (int k = seg; k >= 0; k--)
		{
			const float a = float(k) / seg * 1.5707963f;
			c.push_back(ha - r * (1 - std::cos(a)));
		}
		return c;
	};
	struct FaceDef
	{
		Face id;
		V3 n, u, v; // outward normal; the grid's axes (u x v = n)
		float hu, hv, hn;
	};
	const FaceDef faces[6] = {
		{FFront, {0, 0, -1}, {1, 0, 0}, {0, -1, 0}, half.x, half.y, half.z},
		{FBack, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, half.x, half.y, half.z},
		{FLeft, {-1, 0, 0}, {0, 0, -1}, {0, -1, 0}, half.z, half.y, half.x},
		{FRight, {1, 0, 0}, {0, 0, 1}, {0, -1, 0}, half.z, half.y, half.x},
		{FTop, {0, 1, 0}, {1, 0, 0}, {0, 0, -1}, half.x, half.z, half.y},   // (u x v = n: wound outwards, so
		{FBottom, {0, -1, 0}, {1, 0, 0}, {0, 0, 1}, half.x, half.z, half.y}, //  culling keeps them from outside)
	};
	const V3 lo{-half.x + r, -half.y + r, -half.z + r}, hi{half.x - r, half.y - r, half.z - r};
	for (const FaceDef& f : faces)
	{
		const std::vector<float> cu = coords(f.hu), cv = coords(f.hv);
		const uint16_t base = uint16_t(m.v.size());
		for (float b : cv)
			for (float a : cu)
			{
				const V3 P = f.n * f.hn + f.u * a + f.v * b; // on the sharp box, relative to the centre
				const V3 inner{std::clamp(P.x, lo.x, hi.x), std::clamp(P.y, lo.y, hi.y), std::clamp(P.z, lo.z, hi.z)};
				V3 dir = P - inner;
				const float len = std::sqrt(Dot(dir, dir));
				dir = len > 1e-7f ? dir * (1 / len) : f.n;
				Vert vt;
				vt.p = centre + inner + dir * r;
				vt.n = dir;
				vt.u = (a + f.hu) / (2 * f.hu); // 0..1 across the face, as seen from outside
				vt.v = (b + f.hv) / (2 * f.hv);
				m.v.push_back(vt);
			}
		const int nu = int(cu.size()), nv = int(cv.size());
		for (int j = 0; j + 1 < nv; j++)
			for (int i = 0; i + 1 < nu; i++)
			{
				const uint16_t p00 = uint16_t(base + j * nu + i), p10 = uint16_t(p00 + 1), p01 = uint16_t(p00 + nu), p11 = uint16_t(p01 + 1);
				// u x v = n with v pointing down the face: (p00, p01, p11) winds around n
				m.t.push_back({p00, p01, p11, uint8_t(f.id)});
				m.t.push_back({p00, p11, p10, uint8_t(f.id)});
			}
	}
	return m;
}

// A flat quad (p0 p1 p2 p3 round its edge) facing n, wound so culling keeps it from the side n points to.
void AddQuad(Mesh& m, V3 p0, V3 p1, V3 p2, V3 p3, V3 n, Face f)
{
	const uint16_t b = uint16_t(m.v.size());
	for (const V3& q : {p0, p1, p2, p3})
	{
		Vert vt;
		vt.p = q, vt.n = n, vt.u = 0, vt.v = 0;
		m.v.push_back(vt);
	}
	// visible when Cross(C - A, B - A) points at the eye: the outward normal
	const bool flip = Dot(Cross(p2 - p0, p1 - p0), n) < 0;
	if (!flip)
	{
		m.t.push_back({b, uint16_t(b + 1), uint16_t(b + 2), uint8_t(f)});
		m.t.push_back({b, uint16_t(b + 2), uint16_t(b + 3), uint8_t(f)});
	}
	else
	{
		m.t.push_back({b, uint16_t(b + 2), uint16_t(b + 1), uint8_t(f)});
		m.t.push_back({b, uint16_t(b + 3), uint16_t(b + 2), uint8_t(f)});
	}
}

// The connector's opening and board (object units: height 1; the label is on the -z side).
constexpr float kOpenX0 = -0.52f, kOpenX1 = 0.67f, kOpenZ = 0.075f, kOpenDepth = 0.12f;
constexpr float kBoardX0 = -0.36f, kBoardX1 = 0.51f, kBoardZ0 = -0.017f, kBoardZ1 = -0.005f, kBoardY0 = 0.018f;

Mesh CartridgeMesh()
{
	Mesh m = RoundedBox(1.52f, 0.24f, 0.035f, 5);
	// drop the bottom's flat middle (the triangles of FBottom lying wholly inside the rounding)
	const float hx = m.w / 2 - 0.035f, hz = m.d / 2 - 0.035f;
	std::vector<Tri> keep;
	for (const Tri& t : m.t)
	{
		bool flat_middle = t.face == FBottom;
		for (uint16_t i : {t.a, t.b, t.c})
		{
			const V3& q = m.v[i].p;
			flat_middle = flat_middle && q.y < 1e-4f && std::fabs(q.x) <= hx + 1e-4f && std::fabs(q.z) <= hz + 1e-4f;
		}
		if (!flat_middle)
			keep.push_back(t);
	}
	m.t = keep;
	const V3 down{0, -1, 0};
	// the bottom round the opening: four pieces filling the old flat middle
	AddQuad(m, {-hx, 0, -hz}, {kOpenX0, 0, -hz}, {kOpenX0, 0, hz}, {-hx, 0, hz}, down, FBottom);   // the wide block
	AddQuad(m, {kOpenX1, 0, -hz}, {hx, 0, -hz}, {hx, 0, hz}, {kOpenX1, 0, hz}, down, FBottom);     // the narrow one
	AddQuad(m, {kOpenX0, 0, -hz}, {kOpenX1, 0, -hz}, {kOpenX1, 0, -kOpenZ}, {kOpenX0, 0, -kOpenZ}, down, FBottom);
	AddQuad(m, {kOpenX0, 0, kOpenZ}, {kOpenX1, 0, kOpenZ}, {kOpenX1, 0, hz}, {kOpenX0, 0, hz}, down, FBottom);
	// the opening: walls up into the body, facing inwards, and its ceiling, facing down
	const float d = kOpenDepth;
	AddQuad(m, {kOpenX0, 0, -kOpenZ}, {kOpenX1, 0, -kOpenZ}, {kOpenX1, d, -kOpenZ}, {kOpenX0, d, -kOpenZ}, {0, 0, 1}, FCavity);
	AddQuad(m, {kOpenX0, 0, kOpenZ}, {kOpenX1, 0, kOpenZ}, {kOpenX1, d, kOpenZ}, {kOpenX0, d, kOpenZ}, {0, 0, -1}, FCavity);
	AddQuad(m, {kOpenX0, 0, -kOpenZ}, {kOpenX0, 0, kOpenZ}, {kOpenX0, d, kOpenZ}, {kOpenX0, d, -kOpenZ}, {1, 0, 0}, FCavity);
	AddQuad(m, {kOpenX1, 0, -kOpenZ}, {kOpenX1, 0, kOpenZ}, {kOpenX1, d, kOpenZ}, {kOpenX1, d, -kOpenZ}, {-1, 0, 0}, FCavity);
	AddQuad(m, {kOpenX0, d, -kOpenZ}, {kOpenX1, d, -kOpenZ}, {kOpenX1, d, kOpenZ}, {kOpenX0, d, kOpenZ}, down, FCavity);
	// the board: a thin plate hanging from the ceiling, its cut edge low in the opening
	const float x0 = kBoardX0, x1 = kBoardX1, z0 = kBoardZ0, z1 = kBoardZ1, y0 = kBoardY0;
	AddQuad(m, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, down, FBoard);            // its cut edge
	AddQuad(m, {x0, y0, z0}, {x1, y0, z0}, {x1, d, z0}, {x0, d, z0}, {0, 0, -1}, FBoard);         // the label's side
	AddQuad(m, {x0, y0, z1}, {x1, y0, z1}, {x1, d, z1}, {x0, d, z1}, {0, 0, 1}, FBoard);          // the back's side
	AddQuad(m, {x0, y0, z0}, {x0, y0, z1}, {x0, d, z1}, {x0, d, z0}, {-1, 0, 0}, FBoard);
	AddQuad(m, {x1, y0, z0}, {x1, y0, z1}, {x1, d, z1}, {x1, d, z0}, {1, 0, 0}, FBoard);
	return m;
}

// Al Alamiah's mark as a height (0..1, the mark 1 on its tile), softened so its edges slope; At(u, v) 0..1 each
struct AlamiahEmboss
{
	static constexpr int N = 56, S = 4; // (sampled 4x finer, blurred)
	std::vector<float> h;
	AlamiahEmboss() : h(size_t(N * S) * (N * S), 0.0f)
	{
		const int M = N * S;
		std::vector<float> a(size_t(M) * M);
		for (int y = 0; y < M; y++)
			for (int x = 0; x < M; x++)
			{
				const char c = kAlamiahLogo[y / S][x / S];
				a[size_t(y) * M + x] = c == '2' ? 1.0f : 0.0f; // (the tile is the raised panel itself)
			}
		// two box blurs (radius 3): soft slopes
		for (int pass = 0; pass < 2; pass++)
		{
			std::vector<float> b(a.size());
			for (int y = 0; y < M; y++)
				for (int x = 0; x < M; x++)
				{
					float s2 = 0;
					int n = 0;
					for (int dx = -3; dx <= 3; dx++)
						if (x + dx >= 0 && x + dx < M)
							s2 += a[size_t(y) * M + x + dx], n++;
					b[size_t(y) * M + x] = s2 / n;
				}
			for (int y = 0; y < M; y++)
				for (int x = 0; x < M; x++)
				{
					float s2 = 0;
					int n = 0;
					for (int dy = -3; dy <= 3; dy++)
						if (y + dy >= 0 && y + dy < M)
							s2 += b[size_t(y + dy) * M + x], n++;
					a[size_t(y) * M + x] = s2 / n;
				}
		}
		h = a;
	}
	float At(float u, float v) const
	{
		const int M = N * S;
		const float fx = std::clamp(u, 0.0f, 1.0f) * (M - 1), fy = std::clamp(v, 0.0f, 1.0f) * (M - 1);
		const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, M - 1), y1 = std::min(y0 + 1, M - 1);
		const float tx = fx - x0, ty = fy - y0;
		const float a0 = h[size_t(y0) * M + x0] * (1 - tx) + h[size_t(y0) * M + x1] * tx;
		const float a1 = h[size_t(y1) * M + x0] * (1 - tx) + h[size_t(y1) * M + x1] * tx;
		return a0 * (1 - ty) + a1 * ty;
	}
};
const AlamiahEmboss& AlamiahLogo()
{
	static const AlamiahEmboss e;
	return e;
}

// Al Alamiah's cartridge: its front outline (rounded top corners, nearly square bottom ones) pushed back into a body
// with rounded edges, the same size as the Konami cartridge (the shelf's spacing and sprites stay as they are), and the
// same connector opening underneath.
Mesh AlamiahMesh()
{
	Mesh m;
	const float w = 1.52f, d = 0.24f, hx = w / 2, hd = d / 2;
	const float rt = 0.13f, rb = 0.022f, e = 0.03f; // top corners, bottom corners, the edge's rounding
	m.w = w, m.h = 1, m.d = d;
	struct OP
	{
		float x, y, nx, ny;
		uint8_t face;
	};
	std::vector<OP> ol; // anticlockwise seen from the front... (x right, y up), each with its outward normal
	auto arc = [&](float cx, float cy, float r, float a0, float a1, int n, uint8_t face) {
		for (int i = 0; i <= n; i++)
		{
			const float a = a0 + (a1 - a0) * float(i) / n;
			ol.push_back({cx + r * std::cos(a), cy + r * std::sin(a), std::cos(a), std::sin(a), face});
		}
	};
	const float pi = 3.14159265f;
	// the bottom, left to right (the connector opening's ends as points of their own)
	arc(-hx + rb, rb, rb, pi, 1.5f * pi, 4, FLeft);
	ol.push_back({kOpenX0, 0, 0, -1, FBottom});
	ol.push_back({kOpenX1, 0, 0, -1, FBottom});
	arc(hx - rb, rb, rb, 1.5f * pi, 2 * pi, 4, FRight);
	arc(hx - rt, 1 - rt, rt, 0, 0.5f * pi, 12, FTop);
	arc(-hx + rt, 1 - rt, rt, 0.5f * pi, pi, 12, FTop);
	const int n = int(ol.size());
	// rings round the outline: the front's rounded edge (4 steps), the back's (4 steps); ring 0 lies in the front face
	constexpr int kSteps = 4;
	std::vector<float> ring_z, ring_c, ring_s; // z, and the normal's front (cos) / outward (sin) parts
	for (int k = 0; k <= kSteps; k++)
	{
		const float a = 0.5f * pi * float(k) / kSteps;
		ring_z.push_back(-hd + e - e * std::cos(a)), ring_c.push_back(std::cos(a)), ring_s.push_back(std::sin(a));
	}
	for (int k = kSteps; k >= 0; k--)
	{
		const float a = 0.5f * pi * float(k) / kSteps;
		ring_z.push_back(hd - e + e * std::cos(a)), ring_c.push_back(-std::cos(a)), ring_s.push_back(std::sin(a));
	}
	const int nr = int(ring_z.size());
	auto tri = [&](uint16_t a, uint16_t b, uint16_t c, uint8_t face, V3 out) {
		const V3 &A = m.v[a].p, &B = m.v[b].p, &Cc = m.v[c].p;
		if (Dot(Cross(Cc - A, B - A), out) >= 0)
			m.t.push_back({a, b, c, face});
		else
			m.t.push_back({a, c, b, face});
	};
	const uint16_t base = uint16_t(m.v.size());
	for (int r = 0; r < nr; r++)
		for (const OP& o : ol)
		{
			const float out = e * (ring_s[size_t(r)] - 1); // inset at the faces, flush on the sides
			Vert vt;
			vt.p = {o.x + o.nx * out, o.y + o.ny * out, ring_z[size_t(r)]};
			vt.n = Norm(V3{o.nx * ring_s[size_t(r)], o.ny * ring_s[size_t(r)], -ring_c[size_t(r)]});
			vt.u = vt.v = 0;
			m.v.push_back(vt);
		}
	auto at = [&](int r, int i) { return uint16_t(base + r * n + (i % n)); };
	for (int r = 0; r + 1 < nr; r++)
		for (int i = 0; i < n; i++)
		{
			const OP& o = ol[size_t(i)];
			const OP& o2 = ol[size_t((i + 1) % n)];
			// the wall band between the two edges' roundings: the bottom's middle (the opening) is left out
			const bool wall = r == kSteps;
			if (wall && o.face == FBottom && o2.face == FBottom)
				continue; // (filled round the opening below)
			uint8_t face = r < kSteps ? FFront : r >= kSteps + 1 ? FBack : o.face;
			if (wall)
				face = (o.face == FBottom || o2.face == FBottom) ? FBottom : o.face;
			const V3 nrm = Norm(m.v[at(r, i)].n + m.v[at(r + 1, i + 1)].n);
			tri(at(r, i), at(r + 1, i), at(r + 1, i + 1), face, nrm);
			tri(at(r, i), at(r + 1, i + 1), at(r, i + 1), face, nrm);
		}
	// the front and the back: fans from their middles (the outline is convex)
	for (int side = 0; side < 2; side++)
	{
		const int r = side == 0 ? 0 : nr - 1;
		Vert c;
		c.p = {0, 0.5f, side == 0 ? -hd : hd};
		c.n = {0, 0, side == 0 ? -1.0f : 1.0f};
		c.u = c.v = 0;
		const uint16_t ci = uint16_t(m.v.size());
		m.v.push_back(c);
		for (int i = 0; i < n; i++)
			tri(ci, at(r, i), at(r, i + 1), side == 0 ? FFront : FBack, c.n);
	}
	// round the opening: the bottom's two strips beside it, its walls and ceiling, the board (as the Konami one)
	const V3 down{0, -1, 0};
	const float z0 = -hd + e, z1 = hd - e;
	AddQuad(m, {kOpenX0, 0, z0}, {kOpenX1, 0, z0}, {kOpenX1, 0, -kOpenZ}, {kOpenX0, 0, -kOpenZ}, down, FBottom);
	AddQuad(m, {kOpenX0, 0, kOpenZ}, {kOpenX1, 0, kOpenZ}, {kOpenX1, 0, z1}, {kOpenX0, 0, z1}, down, FBottom);
	const float dd = kOpenDepth;
	AddQuad(m, {kOpenX0, 0, -kOpenZ}, {kOpenX1, 0, -kOpenZ}, {kOpenX1, dd, -kOpenZ}, {kOpenX0, dd, -kOpenZ}, {0, 0, 1}, FCavity);
	AddQuad(m, {kOpenX0, 0, kOpenZ}, {kOpenX1, 0, kOpenZ}, {kOpenX1, dd, kOpenZ}, {kOpenX0, dd, kOpenZ}, {0, 0, -1}, FCavity);
	AddQuad(m, {kOpenX0, 0, -kOpenZ}, {kOpenX0, 0, kOpenZ}, {kOpenX0, dd, kOpenZ}, {kOpenX0, dd, -kOpenZ}, {1, 0, 0}, FCavity);
	AddQuad(m, {kOpenX1, 0, -kOpenZ}, {kOpenX1, 0, kOpenZ}, {kOpenX1, dd, kOpenZ}, {kOpenX1, dd, -kOpenZ}, {-1, 0, 0}, FCavity);
	AddQuad(m, {kOpenX0, dd, -kOpenZ}, {kOpenX1, dd, -kOpenZ}, {kOpenX1, dd, kOpenZ}, {kOpenX0, dd, kOpenZ}, down, FCavity);
	const float bx0 = kBoardX0, bx1 = kBoardX1, bz0 = kBoardZ0, bz1 = kBoardZ1, by0 = kBoardY0;
	AddQuad(m, {bx0, by0, bz0}, {bx1, by0, bz0}, {bx1, by0, bz1}, {bx0, by0, bz1}, down, FBoard);
	AddQuad(m, {bx0, by0, bz0}, {bx1, by0, bz0}, {bx1, dd, bz0}, {bx0, dd, bz0}, {0, 0, -1}, FBoard);
	AddQuad(m, {bx0, by0, bz1}, {bx1, by0, bz1}, {bx1, dd, bz1}, {bx0, dd, bz1}, {0, 0, 1}, FBoard);
	AddQuad(m, {bx0, by0, bz0}, {bx0, by0, bz1}, {bx0, dd, bz1}, {bx0, dd, bz0}, {-1, 0, 0}, FBoard);
	AddQuad(m, {bx1, by0, bz0}, {bx1, by0, bz1}, {bx1, dd, bz1}, {bx1, dd, bz0}, {1, 0, 0}, FBoard);
	return m;
}

// The Holy Quran's cartridge (the user's photo): 1.17:1, a body as deep as the others and on it a deeper head (the label
// on its front), their backs flush - the head stands out at the front, a sloping step under it; the top corners rounded,
// the left one more. Heights over the other cartridges'.
constexpr float kTallH = 1.30f, kTallSplit = 0.74f, kTallD = 0.29f, kTallBodyD = 0.21f;

// One part of it: an outline (x right, y up; rounded corners: bottom rb, top left rtl, top right rtr) pushed back from
// z0 to z1 with rounded edges (e); the front and back filled from their middles (the outline is convex).
void Extrude(Mesh& m, float w, float y0, float y1, float z0, float z1, float rb, float rtl, float rtr, float e)
{
	const float hx = w / 2, pi = 3.14159265f;
	struct OP
	{
		float x, y, nx, ny;
		uint8_t face;
	};
	std::vector<OP> ol;
	auto arc = [&](float cx, float cy, float r, float a0, float a1, int n, uint8_t face) {
		for (int i = 0; i <= n; i++)
		{
			const float a = a0 + (a1 - a0) * float(i) / n;
			ol.push_back({cx + r * std::cos(a), cy + r * std::sin(a), std::cos(a), std::sin(a), face});
		}
	};
	arc(-hx + rb, y0 + rb, rb, pi, 1.5f * pi, 4, FBottom);
	arc(hx - rb, y0 + rb, rb, 1.5f * pi, 2 * pi, 4, FBottom);
	arc(hx - rtr, y1 - rtr, rtr, 0, 0.5f * pi, 10, FTop);
	arc(-hx + rtl, y1 - rtl, rtl, 0.5f * pi, pi, 10, FTop);
	const int n = int(ol.size());
	constexpr int kSteps = 3;
	std::vector<float> rz, rc, rs;
	for (int k = 0; k <= kSteps; k++)
	{
		const float a = 0.5f * pi * float(k) / kSteps;
		rz.push_back(z0 + e - e * std::cos(a)), rc.push_back(std::cos(a)), rs.push_back(std::sin(a));
	}
	for (int k = kSteps; k >= 0; k--)
	{
		const float a = 0.5f * pi * float(k) / kSteps;
		rz.push_back(z1 - e + e * std::cos(a)), rc.push_back(-std::cos(a)), rs.push_back(std::sin(a));
	}
	const int nr = int(rz.size());
	auto tri = [&](uint16_t a, uint16_t b, uint16_t c, uint8_t face, V3 out) {
		const V3 &A = m.v[a].p, &B = m.v[b].p, &Cc = m.v[c].p;
		if (Dot(Cross(Cc - A, B - A), out) >= 0)
			m.t.push_back({a, b, c, face});
		else
			m.t.push_back({a, c, b, face});
	};
	const uint16_t base = uint16_t(m.v.size());
	for (int r = 0; r < nr; r++)
		for (const OP& o : ol)
		{
			const float out = e * (rs[size_t(r)] - 1);
			Vert vt;
			vt.p = {o.x + o.nx * out, o.y + o.ny * out, rz[size_t(r)]};
			vt.n = Norm(V3{o.nx * rs[size_t(r)], o.ny * rs[size_t(r)], -rc[size_t(r)]});
			vt.u = vt.v = 0;
			m.v.push_back(vt);
		}
	auto at = [&](int r, int i) { return uint16_t(base + r * n + (i % n)); };
	for (int r = 0; r + 1 < nr; r++)
		for (int i = 0; i < n; i++)
		{
			const OP& o = ol[size_t(i)];
			const OP& o2 = ol[size_t((i + 1) % n)];
			uint8_t face = r < kSteps ? FFront : r >= kSteps + 1 ? FBack : o.face;
			if (r == kSteps)
			{
				// the sides: the ends between the top's and the bottom's arcs
				const bool vert = std::fabs(o.nx) > 0.7f && std::fabs(o2.nx) > 0.7f;
				face = vert ? (o.x < 0 ? FLeft : FRight) : (o.face == FTop || o2.face == FTop) && o.y > y0 + rb && o2.y > y0 + rb ? FTop : FBottom;
				if (o.face != o2.face && std::fabs(o.nx) > 0.7f)
					face = o.x < 0 ? FLeft : FRight;
			}
			const V3 nrm = Norm(m.v[at(r, i)].n + m.v[at(r + 1, i + 1)].n);
			tri(at(r, i), at(r + 1, i), at(r + 1, i + 1), face, nrm);
			tri(at(r, i), at(r + 1, i + 1), at(r, i + 1), face, nrm);
		}
	for (int side = 0; side < 2; side++)
	{
		const int r = side == 0 ? 0 : nr - 1;
		Vert c;
		c.p = {0, (y0 + y1) / 2, side == 0 ? z0 : z1};
		c.n = {0, 0, side == 0 ? -1.0f : 1.0f};
		c.u = c.v = 0;
		const uint16_t ci = uint16_t(m.v.size());
		m.v.push_back(c);
		for (int i = 0; i < n; i++)
			tri(ci, at(r, i), at(r, i + 1), side == 0 ? FFront : FBack, c.n);
	}
}

Mesh TallMesh()
{
	Mesh m;
	m.w = 1.52f, m.h = kTallH, m.d = kTallD;
	const float hd = kTallD / 2;
	// the body (its top hidden in the head), then the head with the step under its front
	Extrude(m, m.w, 0, kTallSplit + 0.03f, hd - kTallBodyD, hd, 0.02f, 0.02f, 0.02f, 0.025f);
	Extrude(m, m.w, kTallSplit, kTallH, -hd, hd, 0.035f, 0.13f, 0.085f, 0.035f);
	return m;
}

const Mesh& MeshFor(Kind k)
{
	static const Mesh box = RoundedBox(0.72f, 0.17f, 0.012f, 3);
	static const Mesh cart = CartridgeMesh(); // a Konami MSX cartridge: 112 x 73 x 17 mm, its connector open
	static const Mesh floppy = RoundedBox(0.97f, 0.036f, 0.016f, 3);
	static const Mesh wide = RoundedBox(1.36f, 0.27f, 0.014f, 3); // (deeper: a landscape box seen from the front)
	static const Mesh half = RoundedBox(0.72f, 0.085f, 0.01f, 3);  // a clamshell's half
	return k == Kind::Box ? box : k == Kind::BoxWide ? wide : k == Kind::Cartridge ? cart : k == Kind::BoxHalf ? half : floppy;
}

// ---- the back's raised logo: a height map (0..1), drawn once ----
struct Emboss
{
	static constexpr int W = 512, H = 288;
	std::vector<float> h;
	Emboss() : h(size_t(W) * H, 0.0f)
	{
		std::vector<float> m(size_t(W) * H, 0.0f);
		auto seg = [&](float x0, float y0, float x1, float y1, float r) {
			const int bx0 = int(std::min(x0, x1) - r - 1), bx1 = int(std::max(x0, x1) + r + 1);
			const int by0 = int(std::min(y0, y1) - r - 1), by1 = int(std::max(y0, y1) + r + 1);
			const float dx = x1 - x0, dy = y1 - y0, l2 = std::max(1e-6f, dx * dx + dy * dy);
			for (int y = std::max(0, by0); y < std::min(H, by1); y++)
				for (int x = std::max(0, bx0); x < std::min(W, bx1); x++)
				{
					const float t = std::clamp(((x - x0) * dx + (y - y0) * dy) / l2, 0.0f, 1.0f);
					const float ex = x - (x0 + t * dx), ey = y - (y0 + t * dy);
					if (ex * ex + ey * ey <= r * r)
						m[size_t(y) * W + x] = 1;
				}
		};
		// the frame around the letters
		const float fx0 = 40, fx1 = 472, fy0 = 14, fy1 = 128, fr = 6;
		seg(fx0, fy0, fx1, fy0, fr), seg(fx1, fy0, fx1, fy1, fr), seg(fx1, fy1, fx0, fy1, fr), seg(fx0, fy1, fx0, fy0, fr);
		// "MSX": the lettering, filling the frame
		{
			const float lx0 = 66, lx1 = 446, ly0 = 28, ly1 = 114;
			for (int y = int(ly0); y < int(ly1); y++)
				for (int x = int(lx0); x < int(lx1); x++)
				{
					const float u = (x + 0.5f - lx0) / (lx1 - lx0) * kMsxLogoW, v = (y + 0.5f - ly0) / (ly1 - ly0) * kMsxLogoH;
					const int iu = std::clamp(int(u), 0, kMsxLogoW - 1), iv = std::clamp(int(v), 0, kMsxLogoH - 1);
					m[size_t(y) * W + x] = std::max(m[size_t(y) * W + x], kMsxLogo[size_t(iv) * kMsxLogoW + iu] / 255.0f);
				}
		}
		// soften the edges (two box passes)
		for (int pass = 0; pass < 1; pass++)
		{
			std::vector<float> t(m.size());
			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++)
				{
					float a = 0;
					int n = 0;
					for (int k = -1; k <= 1; k++)
					{
						const int xx = std::clamp(x + k, 0, W - 1);
						a += m[size_t(y) * W + xx], n++;
					}
					t[size_t(y) * W + x] = a / n;
				}
			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++)
				{
					float a = 0;
					int n = 0;
					for (int k = -1; k <= 1; k++)
					{
						const int yy = std::clamp(y + k, 0, H - 1);
						a += t[size_t(yy) * W + x], n++;
					}
					m[size_t(y) * W + x] = a / n;
				}
		}
		h.swap(m);
	}
	float At(float u, float v) const // u, v 0..1 over the map
	{
		const int x = std::clamp(int(u * W), 0, W - 1), y = std::clamp(int(v * H), 0, H - 1);
		return h[size_t(y) * W + x];
	}
};
const Emboss& Logo()
{
	static const Emboss* e = new Emboss();
	return *e;
}

inline float Hash2(float x, float y)
{
	const float d = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
	return d - std::floor(d);
}

// ---- textures ----
inline float Lin(uint32_t c, int sh) // sRGB-ish byte -> linear (squared)
{
	const float v = float((c >> sh) & 0xff) * (1.0f / 255);
	return v * v;
}
inline V3 Unpack(uint32_t c)
{
	return {Lin(c, 0), Lin(c, 8), Lin(c, 16)};
}

V3 Sample(const Texture* t, float u, float v, int lod)
{
	if (!t || t->levels.empty())
		return {0.1f, 0.1f, 0.1f};
	const Texture::Level& L = t->levels[size_t(std::clamp(lod, 0, int(t->levels.size()) - 1))];
	const float x = std::clamp(u, 0.0f, 1.0f) * L.w - 0.5f, y = std::clamp(v, 0.0f, 1.0f) * L.h - 0.5f;
	const int x0 = std::clamp(int(std::floor(x)), 0, L.w - 1), y0 = std::clamp(int(std::floor(y)), 0, L.h - 1);
	const int x1 = std::min(x0 + 1, L.w - 1), y1 = std::min(y0 + 1, L.h - 1);
	const float fx = std::clamp(x - std::floor(x), 0.0f, 1.0f), fy = std::clamp(y - std::floor(y), 0.0f, 1.0f);
	const V3 a = Unpack(L.px[size_t(y0) * L.w + x0]), b = Unpack(L.px[size_t(y0) * L.w + x1]);
	const V3 c = Unpack(L.px[size_t(y1) * L.w + x0]), d = Unpack(L.px[size_t(y1) * L.w + x1]);
	return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

int Lod(const Texture* t, float screen_px)
{
	if (!t || t->levels.empty() || screen_px <= 1)
		return 0;
	int l = 0;
	float th = float(t->levels[0].h);
	while (l + 1 < int(t->levels.size()) && th > screen_px * 1.6f)
	{
		th *= 0.5f;
		l++;
	}
	return l;
}

// ---- the camera and the object's placement ----
struct Xform
{
	float m[3][3]; // object -> world rotation, scaled
	V3 t;
	V3 Apply(V3 p) const
	{
		return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + t.x, m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + t.y,
			m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + t.z};
	}
	V3 Rotate(V3 n) const // unit normals (the scale is uniform)
	{
		return Norm({m[0][0] * n.x + m[0][1] * n.y + m[0][2] * n.z, m[1][0] * n.x + m[1][1] * n.y + m[1][2] * n.z,
			m[2][0] * n.x + m[2][1] * n.y + m[2][2] * n.z});
	}
};

Xform Place(const Pose& p, bool mirror)
{
	const float cy = std::cos(p.yaw), sy = std::sin(p.yaw), cp = std::cos(p.pitch), sp = std::sin(p.pitch);
	// yaw around y, then pitch around x (tilting back around the base), then scale
	const float R[3][3] = {{cy, sy * sp, sy * cp}, {0, cp, -sp}, {-sy, cy * sp, cy * cp}};
	Xform x;
	const float s = p.height;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			x.m[i][j] = R[i][j] * s;
	x.t = {p.x, p.lift, p.z};
	if (mirror) // reflected in the floor
	{
		for (int j = 0; j < 3; j++)
			x.m[1][j] = -x.m[1][j];
		x.t.y = -p.lift;
	}
	return x;
}

struct Proj
{
	float sx, sy, iz; // screen position, 1/z
};
inline Proj Project(const View& v, V3 w)
{
	const float iz = 1.0f / std::max(w.z, 1.0f);
	return {v.w * 0.5f + v.focal * w.x * iz, v.horizon - v.focal * (w.y - v.eye_y) * iz, iz};
}

// ---- shading: a product shot ----
struct Material
{
	V3 albedo;
	float ks;   // the lights' highlight
	int shin;   // its sharpness: 2^shin
	float sheen, env; // a broad highlight; the studio's reflection (glossy surfaces)
};

const V3 kKeyDir = Norm({-0.45f, 0.78f, -0.55f}), kFillDir = Norm({0.75f, 0.25f, -0.45f}), kRimDir = Norm({0.25f, 0.55f, 0.85f});
const V3 kKey{1.05f, 1.0f, 0.94f}, kFill{0.30f, 0.33f, 0.40f}, kRim{0.55f, 0.6f, 0.75f};
const V3 kSky{0.16f, 0.17f, 0.22f}, kGround{0.05f, 0.045f, 0.05f};

inline float Smooth(float a, float b, float x)
{
	const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3 - 2 * t);
}

// What a glossy surface mirrors: a dark studio with a big softbox up to the left and a strip light to the right.
inline V3 Studio(V3 r)
{
	const float box = Smooth(0.05f, 0.35f, r.y) * (1 - Smooth(0.18f, 0.42f, std::fabs(r.x + 0.42f))) * (1 - Smooth(0.2f, 0.6f, r.z));
	const float strip = Smooth(-0.1f, 0.3f, r.y) * (1 - Smooth(0.02f, 0.07f, std::fabs(r.x - 0.62f))) * 0.6f;
	const float floor_glow = Smooth(0.0f, -0.6f, r.y) * 0.05f;
	return V3{1.0f, 0.98f, 0.95f} * (box * 1.8f + strip) + V3{0.25f, 0.2f, 0.4f} * floor_glow + V3{0.012f, 0.012f, 0.018f};
}

inline float PowSq(float x, int k)
{
	for (int i = 0; i < k; i++)
		x *= x;
	return x;
}

V3 Light(const Material& m, V3 n, V3 view, bool simple)
{
	V3 c = Mul(m.albedo, kSky * (0.5f + 0.5f * n.y) + kGround * (0.5f - 0.5f * n.y));
	const V3 dirs[3] = {kKeyDir, kFillDir, kRimDir};
	const V3 cols[3] = {kKey, kFill, kRim};
	for (int i = 0; i < 3; i++)
	{
		const float nl = Dot(n, dirs[i]);
		if (nl <= 0)
			continue;
		c = c + Mul(m.albedo, cols[i] * (i == 2 ? nl * 0.35f : nl));
		if (simple)
			continue;
		const V3 h = dirs[i] + view;
		const float nh = std::max(0.0f, Dot(n, h)) / std::sqrt(Dot(h, h));
		const float nh2 = nh * nh;
		c = c + cols[i] * (m.ks * PowSq(nh, m.shin) + m.sheen * nh2 * nh2 * nh2);
	}
	if (simple)
		return c;
	const float nv = std::max(0.0f, Dot(n, view));
	const float f1 = 1 - nv, f = f1 * f1 * f1 * f1 * f1;
	const V3 r = n * (2 * nv) - view; // the mirror direction
	return c + Studio(r) * (m.env * (0.06f + 0.94f * f));
}

inline uint32_t Out(V3 c, float bright)
{
	auto ch = [&](float v) {
		v = std::max(0.0f, v * bright);
		v = v / (1 + v * 0.18f); // a gentle shoulder: highlights don't clip hard
		return uint32_t(std::min(255.0f, std::sqrt(v) * 255.0f + 0.5f));
	};
	return 0xff000000u | ch(c.z) << 16 | ch(c.y) << 8 | ch(c.x);
}

struct Shader
{
	Kind kind;
	const Art* art;
	int lod_front = 0, lod_spine = 0, lod_back = 0, lod_label = 0, lod_disk = 0;
	float shutter = 0; // a floppy's shutter, slid open
	float mw = 1, md = 1, mh = 1; // the mesh's width, depth and height (over the cartridges' height)

	// colour of a point: its face, object position and normal; world normal; view direction
	bool simple = false; // the reflection: no shine
	V3 operator()(uint8_t face, V3 op, V3 wn_geo, const Xform& x, V3 view, V3 on) const
	{
		Material m{{0.006f, 0.006f, 0.007f}, 0.6f, 6, 0.03f, 0.55f}; // glossy black plastic
		// (a coloured plastic of its own: the black's albedo scaled)
		auto plastic = [&](Material& mm) {
			if (art && art->tinted)
				mm.albedo = V3{mm.albedo.x * art->tint[0], mm.albedo.y * art->tint[1], mm.albedo.z * art->tint[2]};
		};
		plastic(m);
		V3 emit{0, 0, 0}; // light of its own (the contacts, seen from below the studio lights)
		V3 n = wn_geo;
		const float hx = mw / 2;
		if ((kind == Kind::BoxHalf) && face == FFront && art->half)
		{
			// a clamshell's inside, facing us when it is open (u across, v down the face)
			const float u = (op.x + hx) / mw, v = 1 - op.y;
			auto inside = [](float a, float lo, float hi, float soft) { // 1 inside [lo, hi], soft edges
				return std::clamp(std::min(a - lo, hi - a) / soft + 0.5f, 0.0f, 1.0f);
			};
			auto rect = [&](float uu, float vv, float u0, float v0, float u1, float v1, float soft) {
				return inside(uu, u0, u1, soft / 0.72f) * inside(vv, v0, v1, soft);
			};
			m = {{0.009f, 0.009f, 0.010f}, 0.42f, 6, 0.03f, 0.45f}; // the case's black plastic
			plastic(m);
			if (art->half == 3)
			{
				// a normal box's insert: a raised rim, one big recess the cartridge lies in (a finger notch at its top
				// edge), a raised plate low down (the maker's name is moulded there); 0 the rim's top, below it deeper
				auto ins = [&](float uu, float vv) {
					float hh = -0.35f * rect(uu, vv, 0.055f, 0.035f, 0.945f, 0.965f, 0.008f);   // inside the rim
					hh -= 0.75f * rect(uu, vv, 0.12f, 0.10f, 0.88f, 0.80f, 0.008f);            // the recess
					const float d = std::sqrt((uu - 0.5f) * (uu - 0.5f) * 0.5184f + (vv - 0.10f) * (vv - 0.10f));
					hh -= 0.6f * std::clamp((0.06f - d) / 0.008f + 0.5f, 0.0f, 1.0f);           // the finger notch
					hh += 0.25f * rect(uu, vv, 0.30f, 0.85f, 0.70f, 0.92f, 0.005f);            // the plate
					return hh;
				};
				const float e = 0.004f;
				const float hh = ins(u, v);
				const float gu = (ins(u + e, v) - ins(u - e, v)) / (2 * e), gv = (ins(u, v + e) - ins(u, v - e)) / (2 * e);
				n = x.Rotate(Norm(V3{-gu * 0.035f, gv * 0.035f, -1}));
				const float depth = std::clamp(-hh / 1.3f, 0.0f, 1.0f);
				m.albedo = m.albedo * (1.0f - 0.55f * depth);
				m.env *= 1.0f - 0.6f * depth;
				m.ks *= 1.0f - 0.4f * depth;
			}
			else if (art->half == 1)
			{
				// the lid: its moulded tray (the user's photo) - a rim all round, a deep pocket at the top with a bar across
				// it, a thumb notch at each side twice, the cartridge's well at the foot; 0 the rim's top, below it deeper
				auto tray = [&](float uu, float vv) {
					if (art->tall)
					{
						// the Quran's: a rim, the floor, a raised block across the top half, the cartridge's well below it
						float hq = -0.5f * rect(uu, vv, 0.05f, 0.03f, 0.95f, 0.97f, 0.008f);
						hq += 0.55f * rect(uu, vv, 0.17f, 0.08f, 0.83f, 0.46f, 0.008f);
						hq -= 0.5f * rect(uu, vv, 0.11f, 0.52f, 0.89f, 0.94f, 0.006f);
						return hq;
					}
					float hh = -0.5f * rect(uu, vv, 0.06f, 0.035f, 0.94f, 0.965f, 0.008f);  // the floor inside the rim
					hh -= 0.6f * rect(uu, vv, 0.14f, 0.075f, 0.86f, 0.47f, 0.008f);         // the deep pocket
					hh += 0.5f * rect(uu, vv, 0.17f, 0.15f, 0.83f, 0.205f, 0.006f);         // the bar across it
					hh -= 0.45f * rect(uu, vv, 0.11f, 0.545f, 0.89f, 0.935f, 0.006f);       // the cartridge's well
					for (float cy : {0.40f, 0.745f})
						for (float cx : {0.075f, 0.925f})
						{
							const float d = std::sqrt((uu - cx) * (uu - cx) * 0.5184f + (vv - cy) * (vv - cy));
							hh -= 0.5f * std::clamp((0.045f - d) / 0.008f + 0.5f, 0.0f, 1.0f); // a thumb notch
						}
					return hh;
				};
				const float e = 0.004f;
				const float hh = tray(u, v);
				const float gu = (tray(u + e, v) - tray(u - e, v)) / (2 * e), gv = (tray(u, v + e) - tray(u, v - e)) / (2 * e);
				// the slopes tilt the normal (u along +x, v down = -y); deeper is darker (the walls shade the floors)
				n = x.Rotate(Norm(V3{-gu * 0.035f, gv * 0.035f, -1}));
				const float depth = std::clamp(-hh / 1.6f, 0.0f, 1.0f);
				m.albedo = m.albedo * (1.0f - 0.55f * depth);
				m.env *= 1.0f - 0.6f * depth;
				m.ks *= 1.0f - 0.4f * depth;
			}
			else
			{
				// the back half: the manual (a booklet with the box's art) held under the clear sleeve, a black frame round it
				// (the Quran's: its printed cover nearly to the edges, no sleeve)
				const float bx = art->tall ? 0.035f : 0.075f, by = art->tall ? 0.025f : 0.05f;
				const float b = rect(u, v, bx, by, 1 - bx, 1 - by, art->tall ? 0.003f : 0.004f);
				if (b > 0.01f && art->front)
				{
					const float bu = (u - bx) / (1 - 2 * bx), bv = (v - by) / (1 - 2 * by);
					const V3 paper = Sample(art->front.get(), bu, bv, lod_front);
					m.albedo = m.albedo * (1 - b) + paper * b;
					if (art->tall)
						m.ks = 0.08f, m.env = 0.012f, m.sheen = 0.12f;
					else
						m.ks = 0.16f, m.env = 0.025f, m.sheen = 0.22f; // under the sleeve: a clear gloss
				}
				else
				{
					// the frame: the booklet's shadow just outside it
					const float sh = rect(u, v, 0.06f, 0.035f, 0.94f, 0.965f, 0.01f);
					m.albedo = m.albedo * (1 - 0.5f * sh);
				}
			}
		}
		else if (kind == Kind::Box || kind == Kind::BoxWide || kind == Kind::BoxHalf)
		{
			// printed card with a gloss coat
			m = {{0.2f, 0.2f, 0.2f}, 0.12f, 6, 0.02f, 0.38f}; // printed card with a gloss coat
			const float u = (op.x + hx) / mw, v = 1 - op.y;
			switch (face)
			{
				case FFront: m.albedo = Sample(art->front.get(), u, v, lod_front); break;
				case FBack: m.albedo = Sample(art->back.get(), 1 - u, v, lod_back); break;
				case FLeft:
				case FRight:
				{
					if (art->clamshell && (face == FRight || art->half == 1 || art->half == 3))
					{
						// the side it opens from: bare black plastic, the two halves meeting in a seam down the middle,
						// each half's edge rounded into a low ridge beside it
						m = {{0.008f, 0.008f, 0.009f}, 0.5f, 6, 0.03f, 0.5f};
						plastic(m);
						const float zc = op.z / (md / 2); // -1 .. 1 across the side
						const V3 fa{0, 0, 1}, fn{1, 0, 0};
						if (std::fabs(zc) < 0.07f)
						{
							m.albedo = m.albedo * 0.3f; // the gap between the halves
							m.env *= 0.2f, m.ks *= 0.3f;
						}
						else
						{
							const float r = std::fabs(zc) - 0.07f; // from the gap out
							if (r < 0.35f)
								n = x.Rotate(Norm(fn + fa * ((zc > 0 ? -1.0f : 1.0f) * 0.9f * (1 - r / 0.35f)))); // the ridges' rounded edges
						}
						break;
					}
					const float s = (op.z + md / 2) / md;
					m.albedo = Sample(art->spine.get(), face == FLeft ? 1 - s : s, v, lod_spine);
					break;
				}
				default:
				{
					if (art->clamshell)
					{
						m = {{0.008f, 0.008f, 0.009f}, 0.5f, 6, 0.03f, 0.5f}; // the clamshell's black plastic
						plastic(m);
						break;
					}
					const Texture* t = art->front.get();
					m.albedo = t ? Unpack(t->average) * 0.8f : V3{0.05f, 0.05f, 0.05f};
					break;
				}
			}
		}
		else if (kind == Kind::Cartridge)
		{
			// glossy black plastic; the front: a plain strip on the left (two round holes, a seam), the label to its right
			const bool konami = art->konami && !art->alamiah;
			const bool alamiah = art->alamiah;
			const float strip = konami ? -hx + 0.31f : -hx;
			float lx0 = konami ? strip : -hx + 0.07f, lx1 = konami ? hx - 0.035f : hx - 0.07f;
			float ly0 = konami ? 0.055f : 0.09f, ly1 = konami ? 0.985f : 0.90f;
			if (alamiah)
			{
				// the label across the top two thirds, clear of the holes on the left
				lx0 = -hx + 0.205f, lx1 = hx - 0.205f, ly0 = 0.37f, ly1 = 0.985f;
				// matte grained plastic: dull, and its grain scattering the light
				m = {{0.0062f, 0.0062f, 0.007f}, 0.12f, 3, 0.009f, 0.13f};
				const float gx = Hash2(std::floor(op.x * 900), std::floor(op.y * 900 + op.z * 900)) - 0.5f;
				const float gy = Hash2(std::floor(op.y * 900 + 7), std::floor(op.x * 900 + op.z * 900 + 3)) - 0.5f;
				const float gz = Hash2(std::floor(op.z * 900 + 11), std::floor(op.x * 900 + op.y * 900 + 5)) - 0.5f;
				n = Norm(n + V3{gx, gy, gz} * 0.16f);
			}
			if (art->tall)
			{
				// the Quran's: the label on the head, plastic either side (more on the right); matte grained plastic
				lx0 = -hx + 0.13f, lx1 = hx - 0.22f, ly0 = kTallSplit + 0.045f, ly1 = kTallH - 0.035f;
				m = {{0.0062f, 0.0062f, 0.007f}, 0.12f, 3, 0.009f, 0.13f};
				plastic(m);
				const float gx = Hash2(std::floor(op.x * 900), std::floor(op.y * 900 + op.z * 900)) - 0.5f;
				const float gy = Hash2(std::floor(op.y * 900 + 7), std::floor(op.x * 900 + op.z * 900 + 3)) - 0.5f;
				n = Norm(n + V3{gx, gy, 0} * 0.12f);
			}
			else if (art->own_label)
			{
				// an Arabic cartridge's label: up to the top edge, nearly edge to edge; Al Alamiah's ends higher
				lx0 = -hx + 0.05f, lx1 = hx - 0.05f, ly1 = 0.985f;
				if (art->alamiah_soft)
					ly0 = 0.28f;
			}
			// the view direction in object space (into the surface)
			const V3 vo = Norm({-(x.m[0][0] * view.x + x.m[1][0] * view.y + x.m[2][0] * view.z),
				-(x.m[0][1] * view.x + x.m[1][1] * view.y + x.m[2][1] * view.z), -(x.m[0][2] * view.x + x.m[1][2] * view.y + x.m[2][2] * view.z)});
			// A round hole going into the surface, traced: the view ray enters at (a, b) and meets the cylinder's wall or
			// its floor. (a, b): the point in the face's plane; (da, db, dn): the ray in the face's frame (dn > 0 goes in);
			// fa, fb, fn: the face's axes in object space, for the wall's normal.
			auto hole = [&](float cx, float cy, float r, float depth, float a, float b, float da, float db, float dn, V3 fa, V3 fb, V3 fn) -> bool {
				const float ox = a - cx, oy = b - cy, d2 = ox * ox + oy * oy;
				const float rim = r * 1.18f;
				if (d2 > rim * rim)
					return false;
				const float d = std::sqrt(d2);
				if (d >= r)
				{
					// the chamfer around the opening
					const float k = (d - r) / (rim - r);
					n = x.Rotate(Norm(fn * -1.0f + (fa * (ox / d) + fb * (oy / d)) * (0.9f * (1 - k))));
					return true;
				}
				// where the ray leaves the circle (it starts inside): |o + t (da, db)| = r
				const float A = da * da + db * db, B = 2 * (ox * da + oy * db), C = d2 - r * r;
				float t = 1e9f;
				if (A > 1e-9f)
					t = (-B + std::sqrt(std::max(0.0f, B * B - 4 * A * C))) / (2 * A);
				const float h = t * dn; // how deep it is there
				float ao;
				V3 nn;
				if (h < depth)
				{
					// the wall: facing the axis
					const float hx2 = ox + t * da, hy2 = oy + t * db;
					nn = (fa * (-hx2 / r) + fb * (-hy2 / r));
					const float q = h / depth;
					ao = 0.55f * (1 - q) * (1 - q) + 0.08f;
				}
				else
				{
					nn = fn * -1.0f; // the floor, facing out
					ao = 0.06f;
				}
				n = x.Rotate(Norm(nn));
				m.albedo = m.albedo * ao;
				m.ks *= ao;
				m.env *= ao * 0.5f;
				m.sheen *= ao;
				return true;
			};
			// a groove (a crease): darker inside, its two walls catching the light. g: across it (0 at its middle)
			auto groove = [&](float g, float w, V3 across, V3 fn) -> bool {
				if (std::fabs(g) > w)
					return false;
				const float k = std::fabs(g) / w;
				if (k > 0.55f)
					n = x.Rotate(Norm(fn + across * (g > 0 ? -0.9f : 0.9f))); // the walls
				else
				{
					m.albedo = m.albedo * 0.5f;
					m.env *= 0.35f;
					m.ks *= 0.5f;
				}
				return true;
			};
			if (face == FFront)
			{
				// (the Al Alamiah label's corners are rounded)
				const float lr = alamiah ? 0.022f : 0.0f;
				const float cxr = op.x < lx0 + lr ? lx0 + lr - op.x : op.x > lx1 - lr ? op.x - (lx1 - lr) : 0.0f;
				const float cyr = op.y < ly0 + lr ? ly0 + lr - op.y : op.y > ly1 - lr ? op.y - (ly1 - lr) : 0.0f;
				const bool in_corner = lr > 0 && cxr * cxr + cyr * cyr > lr * lr;
				if (op.x > lx0 && op.x < lx1 && op.y > ly0 && op.y < ly1 && !in_corner)
				{
					m = {Sample(art->label.get(), (op.x - lx0) / (lx1 - lx0), 1 - (op.y - ly0) / (ly1 - ly0), lod_label), 0.2f, 7, 0.02f,
						0.3f};
					// the label's edge: printed paper on the plastic, a hair of shadow
					const float e = std::min({op.x - lx0, lx1 - op.x, op.y - ly0, ly1 - op.y});
					if (e < 0.006f)
						m.albedo = m.albedo * (0.6f + 0.4f * e / 0.006f);
				}
				else if (alamiah || (art->tall && op.y < kTallSplit))
				{
					const V3 fa{1, 0, 0}, fb{0, 1, 0}, fn{0, 0, 1};
					// the two holes down the left, right through the body (the Quran's: one, low)
					if (art->tall ? !hole(-hx + 0.10f, 0.29f, 0.042f, md * 0.6f, op.x, op.y, vo.x, vo.y, vo.z, fa, fb, fn)
								  : (!hole(-hx + 0.118f, 0.915f, 0.046f, md * 0.9f, op.x, op.y, vo.x, vo.y, vo.z, fa, fb, fn) &&
									 !hole(-hx + 0.118f, 0.255f, 0.046f, md * 0.9f, op.x, op.y, vo.x, vo.y, vo.z, fa, fb, fn)))
					{
						// the raised panel at the bottom right: the logo, then grip ribs to the end
						const float px0 = hx - 0.29f, px1 = hx - 0.012f, py0 = 0.125f, py1 = 0.285f;
						if (op.x > px0 && op.x < px1 && op.y > py0 && op.y < py1)
						{
							const float ex = std::min(op.x - px0, px1 - op.x), ey = std::min(op.y - py0, py1 - op.y);
							const float edge = std::min(ex, ey);
							if (edge < 0.006f)
							{
								// its edge: a little wall catching the light
								const bool side = ex < ey;
								n = x.Rotate(Norm(V3{0, 0, -1} + (side ? fa * (op.x - px0 < px1 - op.x ? -1.2f : 1.2f)
																	  : fb * (op.y - py0 < py1 - op.y ? -1.2f : 1.2f))));
							}
							else
							{
								m.albedo = V3{0.011f, 0.011f, 0.012f};
								m.ks = 0.22f, m.env = 0.02f;
								plastic(m);
								// the logo: Al Alamiah's mark, raised (traced from the label): its slopes tilt the normal
								const float lw = 0.13f, lu = (op.x - (px0 + 0.02f)) / lw, lv = 1 - (op.y - (py0 + 0.012f)) / (py1 - py0 - 0.024f);
								const bool on_logo = lu > 0 && lu < 1 && lv > 0 && lv < 1;
								float hgt = 0;
								if (on_logo)
								{
									const AlamiahEmboss& e = AlamiahLogo();
									const float du = 1.0f / 224, dv = 1.0f / 224;
									hgt = e.At(lu, lv);
									const float gx = e.At(lu + du, lv) - e.At(lu - du, lv), gy = e.At(lu, lv + dv) - e.At(lu, lv - dv);
									n = x.Rotate(Norm(V3{0, 0, -1} + fa * (-gx * 9.0f) + fb * (gy * 9.0f)));
									if (hgt > 0.8f)
									{
										m.albedo = V3{0.016f, 0.016f, 0.018f}; // the mark's top: smoother, a touch brighter
										m.ks = 0.3f, m.env = 0.04f;
										plastic(m);
									}
								}
								// the logo's stripes: across its right half (through the mark's small lobes) and on to the panel's end
								if (op.x > px0 + 0.02f + lw * 0.55f)
									groove(std::fmod(op.y - py0, 0.0123f) - 0.00615f, 0.0029f, fb, V3{0, 0, -1}); // the ribs
							}
						}
						else
							groove(op.y - 0.095f, 0.004f, fb, V3{0, 0, -1}); // the step along the bottom
					}
				}
				else if (art->alamiah_soft && op.y < ly0)
				{
					// Al Alamiah's software cartridge: a round hole low on the left, a recessed panel low on the right
					const V3 fa{1, 0, 0}, fb{0, 1, 0}, fn{0, 0, 1};
					if (!hole(-hx + 0.11f, 0.14f, 0.042f, md * 0.6f, op.x, op.y, vo.x, vo.y, vo.z, fa, fb, fn))
					{
						const float rx0 = hx - 0.47f, rx1 = hx - 0.10f, ry0 = 0.05f, ry1 = 0.235f;
						if (!art->tall && op.x > rx0 && op.x < rx1 && op.y > ry0 && op.y < ry1)
						{
							const float ex = std::min(op.x - rx0, rx1 - op.x), ey = std::min(op.y - ry0, ry1 - op.y);
							if (std::min(ex, ey) < 0.007f)
							{
								// its walls, sloping in
								const bool side = ex < ey;
								n = x.Rotate(Norm(V3{0, 0, -1} + (side ? fa * (op.x - rx0 < rx1 - op.x ? 1.1f : -1.1f)
																	  : fb * (op.y - ry0 < ry1 - op.y ? 1.1f : -1.1f))));
								m.albedo = m.albedo * 0.7f;
							}
							else
							{
								m.albedo = m.albedo * 0.55f; // its floor, in shadow
								m.env *= 0.5f;
							}
						}
					}
				}
				else if (konami && op.x < lx0)
				{
					// two holes in the strip (the front's frame: x right, y up, into the body = +z)
					const V3 fa{1, 0, 0}, fb{0, 1, 0}, fn{0, 0, 1};
					if (!hole(-hx + 0.13f, 0.80f, 0.034f, 0.05f, op.x, op.y, vo.x, vo.y, vo.z, fa, fb, fn) &&
						!hole(-hx + 0.13f, 0.30f, 0.062f, 0.07f, op.x, op.y, vo.x, vo.y, vo.z, fa, fb, fn))
						if (!groove(op.y - 0.90f, 0.0095f, fb, V3{0, 0, -1}) && !groove(op.y - 0.945f, 0.0095f, fb, V3{0, 0, -1}))
							groove(op.x - (strip - 0.035f), 0.007f, fa, V3{0, 0, -1}); // two creases above the top hole; the seam
				}
			}
			else if (face == FLeft || face == FRight)
			{
				// the ends: two rail grooves down them, the halves' seam, and (right) a little hole
				const float sgn = face == FRight ? 1.0f : -1.0f;
				const V3 fa{0, 0, 1}, fb{0, 1, 0}, fn{-sgn, 0, 0}; // in-plane axes; into the body
				(void)fn;
				// the right end: a round boss, slightly raised (its edge catches the light, a soft shadow around it)
				bool on_boss = false;
				if (face == FRight && konami && !alamiah)
				{
					const float bx = op.z - 0.0f, by = op.y - 0.80f, d = std::sqrt(bx * bx + by * by), r = 0.042f;
					if (d < r * 0.82f)
						on_boss = true; // its flat top
					else if (d < r)
					{
						const float k = (d - r * 0.82f) / (r * 0.18f);
						n = x.Rotate(Norm(V3{sgn, 0, 0} + (fa * (bx / d) + fb * (by / d)) * (1.1f * k))); // the rounded edge
						on_boss = true;
					}
					else if (d < r * 1.3f)
					{
						const float k = (d - r) / (r * 0.3f);
						m.albedo = m.albedo * (0.55f + 0.45f * k); // its shadow on the end
						m.env *= 0.5f + 0.5f * k;
					}
				}
				// one crease down the middle of the end
				if (!on_boss && !art->tall && op.y > 0.04f && op.y < 0.97f)
					groove(op.z, 0.0085f, fa, V3{sgn, 0, 0});
			}
			else if (face == FTop)
			{
				if (art->top) // printed (seen from the front: the picture's foot along the front edge)
					m = {Sample(art->top.get(), (op.x + hx) / mw, (md / 2 - op.z) / md, lod_label), 0.25f, 7, 0.03f, 0.3f};
				else
					groove(op.z, 0.0035f, V3{0, 0, 1}, V3{0, 1, 0}); // the halves' seam along the top
			}
			else if (face == FBack && art->rear)
			{
				// its own picture (seen from behind: u runs right to left), matte
				m = {Sample(art->rear.get(), (hx - op.x) / mw, 1 - op.y / mh, lod_label), 0.12f, 3, 0.009f, 0.13f};
			}
			else if (face == FBack)
			{
				// brushed plastic: streaks along its length, some catching the light like scratches
				const float row = op.y * 1.0f;
				const float st = 0.5f * Hash2(std::floor(row * 1400), 1.7f) + 0.3f * Hash2(std::floor(row * 420), 5.3f) +
					0.2f * Hash2(std::floor(row * 110), 9.1f);
				const float broken = 0.75f + 0.25f * Hash2(std::floor(op.x * 9 + row * 37), std::floor(row * 1400));
				const float k = st * broken;
				m = {V3{0.030f, 0.031f, 0.034f} * (0.55f + 0.9f * k), 0.25f + 0.5f * k * k, 4, 0.10f * k, 0.30f + 0.5f * k * k};
				plastic(m);
				// seen from behind: u runs right to left
				const float u = (hx - op.x) / mw, vv = 1 - op.y;
				const V3 fa{-1, 0, 0}, fb{0, 1, 0}, fn{0, 0, -1}; // the back's axes; into the body
				(void)fn;
				{
					// the raised logo, over the middle: its slope tilts the normal
					const float lu0 = 0.20f, lu1 = 0.80f, lv0 = 0.30f, lv1 = 0.96f; // the logo sits a little above the middle
					if (!art->tall && u > lu0 && u < lu1 && vv > lv0 && vv < lv1)
					{
						const Emboss& e = Logo();
						const float eu = (u - lu0) / (lu1 - lu0), ev = (vv - lv0) / (lv1 - lv0);
						const float du = 1.0f / Emboss::W, dv = 1.0f / Emboss::H;
						const float gx = e.At(eu + du, ev) - e.At(eu - du, ev), gy = e.At(eu, ev + dv) - e.At(eu, ev - dv);
						const float hgt = e.At(eu, ev);
						n = x.Rotate(Norm(V3{0, 0, 1} + fa * (-gx * 5.0f) + fb * (gy * 5.0f)));
						if (hgt > 0.6f)
						{
							// the raised tops: smoother, a little brighter than the brushed field
							m.albedo = V3{0.045f, 0.046f, 0.05f};
							m.ks = 0.4f, m.env = 0.45f;
						}
					}
				}
			}
			else if (face == FBottom && !art->tall)
			{
				// the screw post's hole in the wide block, beside the opening
				const float cx = (-hx + kOpenX0) / 2;
				if (op.x < kOpenX0)
					hole(cx, 0.0f, 0.032f, 0.08f, op.x, op.z, vo.x, vo.z, -vo.y, V3{1, 0, 0}, V3{0, 0, 1}, V3{0, 1, 0});
			}
			else if (face == FCavity)
			{
				// inside the opening: dull black, darker deeper in
				const float deep = std::clamp(op.y / kOpenDepth, 0.0f, 1.0f);
				m = {V3{0.008f, 0.008f, 0.009f} * (1 - 0.6f * deep), 0.02f, 4, 0.0f, 0.0f}; // (no studio reflection)
			}
			else if (face == FBoard)
			{
				// the board: green, its cut edge lighter; the contacts on its sides - strips from the edge up, a little
				// gap between them, dark gold; its far end bare (as the photos)
				m = {{0.05f, 0.19f, 0.10f}, 0.3f, 5, 0.03f, 0.2f};
				emit = m.albedo * 0.3f;
				if (on.y < -0.5f)
				{
					m.albedo = {0.13f, 0.34f, 0.20f};
					emit = m.albedo * 0.4f;
				}
				else if (std::fabs(on.z) > 0.5f)
				{
					const float cell = std::fmod((op.x - kBoardX0) * 32.0f, 1.0f);
					if (cell < 0.55f && op.x < kBoardX1 - 0.10f && op.y < kBoardY0 + 0.07f)
					{
						m = {{0.20f, 0.15f, 0.05f}, 0.8f, 6, 0.1f, 0.7f}; // a contact
						emit = V3{0.05f, 0.036f, 0.012f};
					}
				}
			}
		}
		else // floppy
		{
			m = {{0.016f, 0.017f, 0.02f}, 0.45f, 6, 0.04f, 0.7f};
			if (face == FFront || face == FBack)
			{
				const float u = (op.x + hx) / mw, v = 1 - op.y;
				const bool front = face == FFront;
				// the metal shutter across the top middle, its window, the label below
				// the shutter slides left by `off`; its window then lines up with the case's opening and the disk shows
				const float off = 0.15f * std::clamp(shutter, 0.0f, 1.0f), su = u + off;
				if (su > 0.22f && su < 0.78f && v < 0.36f)
				{
					m = {{0.42f, 0.43f, 0.46f}, 0.9f, 5, 0.15f, 1.1f};
					if (su > 0.55f && su < 0.70f && v > 0.05f && v < 0.31f)
					{
						if (u > 0.40f && u < 0.55f)
						{
							// the disk through both windows: brown, a soft sheen across it
							const float t = (u - 0.40f) / 0.15f;
							m = {V3{0.010f, 0.010f, 0.011f} * (0.8f + 0.4f * t), 0.35f, 6, 0.05f, 0.25f}; // very dark
						}
						else
							m.albedo = {0.01f, 0.01f, 0.012f}; // the case behind
					}
					const float brushed = 0.92f + 0.08f * std::sin(op.y * 900.0f);
					m.albedo = m.albedo * brushed;
				}
				else if (u > 0.22f && u < 0.78f && v < 0.36f)
					m.albedo = m.albedo * 0.6f; // the recess the shutter left
				else if (front && u > 0.10f && u < 0.90f && v > 0.42f && v < 0.97f)
				{
					m = {Sample(art->disk_label ? art->disk_label.get() : art->label.get(), (u - 0.10f) / 0.80f, (v - 0.42f) / 0.55f,
							 art->disk_label ? lod_disk : lod_label),
						0.18f, 6, 0.02f, 0.28f};
				}
			}
		}
		(void)on;
		return Light(m, n, view, simple) + emit;
	}
};

// ---- the rasterizer: one object, columns [x0, x1): 4 samples a pixel for its edges, shaded once a pixel ----
// the reflection shows only its lower part: it fades out above this (of the object's height)
constexpr float kMirrorTop = 0.45f;
const float kSampleX[4] = {0.25f, 0.75f, 0.25f, 0.75f}, kSampleY[4] = {0.25f, 0.25f, 0.75f, 0.75f};

// Where an object is drawn: the screen (keep == nullptr) or a sprite, whose pixels say how much of what is under them
// stays (keep, 0..255) and what is added (colour): every step (the shadow darkening, the reflection adding, the body
// covering) keeps that form. (ox, oy): the target's top left on screen; tw x th its size.
struct Target
{
	uint32_t* px;
	uint8_t* keep;
	int stride, ox, oy, tw, th;
};

struct Scratch
{
	std::vector<uint32_t> col; // 4 a pixel
	std::vector<float> depth;
};

void Raster(const Target& tg, const View& v, const Mesh& mesh, const Xform& x, const Shader& sh, int rx0, int rx1, int ry0, int ry1,
	float alpha, float bright, bool reflection, float clip_floor, float height, bool fast)
{
	const int rw = rx1 - rx0, rh = ry1 - ry0;
	if (rw <= 0 || rh <= 0)
		return;
	const int ns = reflection || fast ? 1 : 4; // the reflection is soft anyway, a moving object is moving: one sample
	thread_local Scratch s;
	s.col.assign(size_t(rw) * rh * ns, 0);
	s.depth.assign(size_t(rw) * rh * ns, 0.0f);
	thread_local std::vector<V3> wp, wn;
	thread_local std::vector<Proj> pr;
	wp.resize(mesh.v.size());
	wn.resize(mesh.v.size());
	pr.resize(mesh.v.size());
	for (size_t i = 0; i < mesh.v.size(); i++)
	{
		wp[i] = x.Apply(mesh.v[i].p);
		wn[i] = x.Rotate(mesh.v[i].n);
		pr[i] = Project(v, wp[i]);
	}
	const V3 eye{0, v.eye_y, 0};
	const float sx[4] = {ns == 1 ? 0.5f : kSampleX[0], kSampleX[1], kSampleX[2], kSampleX[3]};
	const float sy[4] = {ns == 1 ? 0.5f : kSampleY[0], kSampleY[1], kSampleY[2], kSampleY[3]};
	for (const Tri& t : mesh.t)
	{
		const V3 &A = wp[t.a], &B = wp[t.b], &C = wp[t.c];
		V3 gn = Cross(C - A, B - A); // the outward normal (the grid winds (v, u+v))
		if (reflection)
			gn = gn * -1.0f; // the mirror turns the winding round
		if (Dot(gn, A - eye) >= 0)
			continue; // facing away
		const Proj &pa = pr[t.a], &pb = pr[t.b], &pc = pr[t.c];
		const float ax = pa.sx - rx0, ay = pa.sy - ry0, bx = pb.sx - rx0, by = pb.sy - ry0, cx = pc.sx - rx0, cy = pc.sy - ry0;
		const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
		if (std::fabs(area) < 1e-8f)
			continue;
		const int minx = std::max(0, int(std::floor(std::min({ax, bx, cx})))), maxx = std::min(rw - 1, int(std::floor(std::max({ax, bx, cx}))));
		const int miny = std::max(0, int(std::floor(std::min({ay, by, cy})))), maxy = std::min(rh - 1, int(std::floor(std::max({ay, by, cy}))));
		if (minx > maxx || miny > maxy)
			continue;
		const float inv = 1.0f / area;
		// barycentric weights as linear functions of the screen position: w = a*x + b*y + c
		const float a0 = (by - cy) * inv, b0 = (cx - bx) * inv, c0 = (bx * cy - by * cx) * inv;
		const float a1 = (cy - ay) * inv, b1 = (ax - cx) * inv, c1 = (cx * ay - cy * ax) * inv;
		const Vert &va = mesh.v[t.a], &vb = mesh.v[t.b], &vc = mesh.v[t.c];
		for (int py = miny; py <= maxy; py++)
			for (int px = minx; px <= maxx; px++)
			{
				const size_t base = (size_t(py) * rw + px) * ns;
				int mask = 0;
				float izs[4];
				for (int k = 0; k < ns; k++)
				{
					const float fx = px + sx[k], fy = py + sy[k];
					const float w0 = a0 * fx + b0 * fy + c0, w1 = a1 * fx + b1 * fy + c1, w2 = 1 - w0 - w1;
					if (w0 < 0 || w1 < 0 || w2 < 0)
						continue;
					const float iz = w0 * pa.iz + w1 * pb.iz + w2 * pc.iz;
					if (iz <= s.depth[base + k])
						continue;
					izs[k] = iz;
					mask |= 1 << k;
				}
				if (!mask)
					continue;
				// shade once: at the pixel's centre, pulled inside the triangle
				const float fx = px + 0.5f, fy = py + 0.5f;
				float w0 = a0 * fx + b0 * fy + c0, w1 = a1 * fx + b1 * fy + c1, w2 = 1 - w0 - w1;
				w0 = std::max(0.0f, w0), w1 = std::max(0.0f, w1), w2 = std::max(0.0f, w2);
				const float ws = w0 + w1 + w2;
				w0 /= ws, w1 /= ws, w2 /= ws;
				const float iz = w0 * pa.iz + w1 * pb.iz + w2 * pc.iz;
				const float pw0 = w0 * pa.iz / iz, pw1 = w1 * pb.iz / iz, pw2 = 1 - pw0 - pw1;
				const V3 op = va.p * pw0 + vb.p * pw1 + vc.p * pw2;
				if (reflection && op.y >= kMirrorTop)
					continue; // faded out
				const V3 world = A * pw0 + B * pw1 + C * pw2;
				if (clip_floor >= 0 && (reflection ? -world.y : world.y) < clip_floor)
					continue;
				const V3 n = Norm(wn[t.a] * pw0 + wn[t.b] * pw1 + wn[t.c] * pw2);
				const V3 view = Norm(eye - world);
				V3 c = sh(t.face, op, n, x, view, va.n);
				if (reflection)
				{
					const float k = std::max(0.0f, 1 - op.y / kMirrorTop); // fades with the height it mirrors
					c = c * (0.3f * k * k);
				}
				const uint32_t out = Out(c, bright);
				for (int k = 0; k < ns; k++)
					if (mask & (1 << k))
					{
						s.col[base + k] = out;
						s.depth[base + k] = izs[k];
					}
			}
	}
	// resolve: average the samples, blend by how many were covered
	const int ty0 = std::max(ry0, tg.oy), ty1 = std::min(ry1, tg.oy + tg.th);
	const int tx0 = std::max(rx0, tg.ox), tx1 = std::min(rx1, tg.ox + tg.tw);
	for (int y = ty0; y < ty1; y++)
	{
		uint32_t* row = tg.px + size_t(y - tg.oy) * tg.stride;
		uint8_t* keep = tg.keep ? tg.keep + size_t(y - tg.oy) * tg.stride : nullptr;
		for (int xx = tx0; xx < tx1; xx++)
		{
			const uint32_t* q = &s.col[(size_t(y - ry0) * rw + (xx - rx0)) * ns];
			uint32_t r = 0, g = 0, b = 0, n = 0;
			for (int k = 0; k < ns; k++)
				if (q[k])
				{
					r += q[k] & 0xff, g += (q[k] >> 8) & 0xff, b += (q[k] >> 16) & 0xff;
					n++;
				}
			if (!n)
				continue;
			const float a = alpha * n / float(ns);
			uint32_t& d = row[xx - tg.ox];
			if (reflection)
			{
				// the reflection adds light to the floor
				const uint32_t nr = std::min(255u, (d & 0xff) + uint32_t(r / n * a)), ng = std::min(255u, ((d >> 8) & 0xff) + uint32_t(g / n * a));
				const uint32_t nb = std::min(255u, ((d >> 16) & 0xff) + uint32_t(b / n * a));
				d = 0xff000000u | nb << 16 | ng << 8 | nr;
			}
			else
			{
				auto mix = [&](uint32_t dc, uint32_t sum) { return uint32_t(dc + (float(sum) / n - dc) * a); };
				d = 0xff000000u | mix((d >> 16) & 0xff, b) << 16 | mix((d >> 8) & 0xff, g) << 8 | mix(d & 0xff, r);
				if (keep)
					keep[xx - tg.ox] = uint8_t(keep[xx - tg.ox] * (1 - a) + 0.5f);
			}
		}
	}
	(void)height;
}

// The object's box on screen (its 8 corners), unclamped: mirror 0 the body, 1 its reflection (from m0 to m1).
bool RawBounds(const View& v, const Mesh& mesh, const Pose& p, int m0, int m1, float& lx, float& ly, float& hx, float& hy)
{
	lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
	for (int mirror = m0; mirror <= m1; mirror++)
	{
		const Xform x = Place(p, mirror != 0);
		for (int i = 0; i < 8; i++)
		{
			const V3 c{(i & 1 ? 0.5f : -0.5f) * mesh.w, i & 2 ? (mirror ? kMirrorTop : mesh.h) : 0.0f, (i & 4 ? 0.5f : -0.5f) * mesh.d};
			const V3 w = x.Apply(c);
			if (w.z < 10)
				return false;
			const Proj q = Project(v, w);
			lx = std::min(lx, q.sx), hx = std::max(hx, q.sx), ly = std::min(ly, q.sy), hy = std::max(hy, q.sy);
		}
	}
	return true;
}

// The soft shadow's box on screen; false: none.
bool ShadowRect(const View& v, const Mesh& mesh, const Pose& p, float& cx, float& cy, float& rx, float& ry, float& strength)
{
	const Xform x = Place(p, false);
	const V3 c = x.Apply({0, 0, 0});
	if (c.z < 10)
		return false;
	const Proj q = Project(v, {c.x, 0, c.z});
	cx = q.sx, cy = q.sy;
	rx = v.focal * mesh.w * p.height * 0.62f / c.z, ry = std::max(4.0f, v.focal * 0.07f * p.height / c.z);
	const float lift = std::max(0.0f, p.lift) / (p.height * 0.5f);
	strength = 0.55f * p.alpha * std::max(0.0f, 1 - lift);
	return strength > 0;
}

// A soft dark pool under the object.
void Shadow(const Target& tg, const View& v, const Mesh& mesh, const Pose& p, int x0, int x1)
{
	float cx, cy, rx, ry, strength;
	if (!ShadowRect(v, mesh, p, cx, cy, rx, ry, strength))
		return;
	const int sy0 = std::max(tg.oy, int(cy - ry * 2)), sy1 = std::min(tg.oy + tg.th, int(cy + ry * 2));
	const int sx0 = std::max(x0, int(cx - rx * 1.3f)), sx1 = std::min(x1, int(cx + rx * 1.3f));
	for (int y = sy0; y < sy1; y++)
	{
		uint32_t* row = tg.px + size_t(y - tg.oy) * tg.stride;
		uint8_t* keep = tg.keep ? tg.keep + size_t(y - tg.oy) * tg.stride : nullptr;
		const float dy = (y - cy) / ry;
		for (int xx = sx0; xx < sx1; xx++)
		{
			const float dx = (xx - cx) / rx;
			const float r2 = dx * dx + dy * dy;
			if (r2 >= 1.69f)
				continue;
			const float k = 1 - strength * std::exp(-r2 * 2.2f);
			uint32_t& d = row[xx - tg.ox];
			d = 0xff000000u | uint32_t(((d >> 16) & 0xff) * k) << 16 | uint32_t(((d >> 8) & 0xff) * k) << 8 | uint32_t((d & 0xff) * k);
			if (keep)
				keep[xx - tg.ox] = uint8_t(keep[xx - tg.ox] * k + 0.5f);
		}
	}
}

// The mesh for this game's object: Al Alamiah's and the Quran's cartridges have their own
const Mesh& MeshOf(Kind k, const Art* art)
{
	static const Mesh alamiah = AlamiahMesh();
	static const Mesh tall = TallMesh();
	if (k == Kind::Cartridge && art)
	{
		if (art->tall)
			return tall;
		if (art->alamiah)
			return alamiah;
	}
	return MeshFor(k);
}

// Columns [x0, x1) of one object into a target: its shadow, its reflection (in its own box), its body.
void DrawTo(const Target& tg, const View& v, Kind k, const Pose& p, const Art& art, int x0, int x1)
{
	x0 = std::max(x0, tg.ox), x1 = std::min(x1, tg.ox + tg.tw);
	if (x0 >= x1)
		return;
	const Mesh& mesh = MeshOf(k, &art);
	Shader sh;
	sh.kind = k;
	sh.art = &art;
	sh.mw = mesh.w, sh.md = mesh.d, sh.mh = mesh.h;
	// texture levels from the object's size on screen
	const float screen_h = v.focal * p.height / std::max(p.z, 10.0f);
	sh.lod_front = Lod(art.front.get(), screen_h);
	sh.lod_back = Lod(art.back.get(), screen_h);
	sh.lod_spine = Lod(art.spine.get(), screen_h);
	sh.lod_label = Lod(art.label.get(), screen_h * 0.6f);
	sh.lod_disk = Lod(art.disk_label.get(), screen_h * 0.6f);
	sh.shutter = p.shutter;
	if (p.shadow)
		Shadow(tg, v, mesh, p, x0, x1);
	for (int mirror = p.reflection ? 1 : 0; mirror >= 0; mirror--)
	{
		float lx, ly, hx, hy;
		if (!RawBounds(v, mesh, p, mirror, mirror, lx, ly, hx, hy))
		{
			if (!mirror)
				return;
			continue;
		}
		const int bx0 = std::max(x0, int(std::floor(lx)) - 1), bx1 = std::min(x1, int(std::ceil(hx)) + 2);
		const int by0 = std::max(tg.oy, int(std::floor(ly)) - 1), by1 = std::min(tg.oy + tg.th, int(std::ceil(hy)) + 2);
		if (bx0 >= bx1 || by0 >= by1)
			continue;
		sh.simple = mirror != 0;
		Raster(tg, v, mesh, Place(p, mirror != 0), sh, bx0, bx1, by0, by1, p.alpha, p.bright, mirror != 0, p.clip_floor, p.height,
			p.fast);
	}
}
} // namespace

std::shared_ptr<Texture> Texture::FromPixels(const uint32_t* abgr, int w, int h)
{
	auto t = std::make_shared<Texture>();
	Level l0;
	l0.w = w, l0.h = h;
	l0.px.assign(abgr, abgr + size_t(w) * h);
	t->levels.push_back(std::move(l0));
	while (t->levels.back().w > 8 && t->levels.back().h > 8 && t->levels.size() < 8)
	{
		const Level& p = t->levels.back();
		Level n;
		n.w = std::max(1, p.w / 2), n.h = std::max(1, p.h / 2);
		n.px.resize(size_t(n.w) * n.h);
		for (int y = 0; y < n.h; y++)
			for (int x = 0; x < n.w; x++)
			{
				uint32_t acc[4] = {0, 0, 0, 0};
				for (int k = 0; k < 4; k++)
				{
					const uint32_t c = p.px[size_t(std::min(2 * y + (k >> 1), p.h - 1)) * p.w + std::min(2 * x + (k & 1), p.w - 1)];
					for (int ch = 0; ch < 4; ch++)
						acc[ch] += (c >> (ch * 8)) & 0xff;
				}
				n.px[size_t(y) * n.w + x] = (acc[0] + 2) / 4 | ((acc[1] + 2) / 4) << 8 | ((acc[2] + 2) / 4) << 16 | ((acc[3] + 2) / 4) << 24;
			}
		t->levels.push_back(std::move(n));
	}
	const Level& last = t->levels.back();
	uint64_t r = 0, g = 0, b = 0;
	for (uint32_t c : last.px)
		r += c & 0xff, g += (c >> 8) & 0xff, b += (c >> 16) & 0xff;
	const uint64_t n = std::max<size_t>(1, last.px.size());
	t->average = 0xff000000u | uint32_t(b / n) << 16 | uint32_t(g / n) << 8 | uint32_t(r / n);
	return t;
}

void Proportions(Kind k, float& w, float& d)
{
	const Mesh& m = MeshFor(k);
	w = m.w, d = m.d;
}

bool Extent(const View& v, Kind k, const Pose& p, int& x0, int& x1)
{
	float lx, ly, hx, hy;
	if (!RawBounds(v, MeshFor(k), p, 0, p.reflection ? 1 : 0, lx, ly, hx, hy))
		return false;
	x0 = std::max(0, int(std::floor(lx)) - 1), x1 = std::min(v.w, int(std::ceil(hx)) + 2);
	return x0 < x1;
}

void Draw(uint32_t* surf, const View& v, Kind k, const Pose& p, const Art& art, int x0, int x1)
{
	const Target tg{surf, nullptr, v.w, 0, 0, v.w, v.h};
	DrawTo(tg, v, k, p, art, x0, x1);
}

bool Prepare(Sprite& s, const View& v, Kind k, const Pose& p, const Art* art)
{
	const Mesh& mesh = MeshOf(k, art);
	float lx, ly, hx, hy;
	s.w = s.h = 0;
	if (!RawBounds(v, mesh, p, 0, p.reflection ? 1 : 0, lx, ly, hx, hy))
		return false;
	float cx, cy, rx, ry, strength;
	if (p.shadow && ShadowRect(v, mesh, p, cx, cy, rx, ry, strength))
	{
		lx = std::min(lx, cx - rx * 1.3f), hx = std::max(hx, cx + rx * 1.3f);
		ly = std::min(ly, cy - ry * 2), hy = std::max(hy, cy + ry * 2);
	}
	s.x0 = int(std::floor(lx)) - 2, s.y0 = std::max(0, int(std::floor(ly)) - 2);
	const int x1 = int(std::ceil(hx)) + 3, y1 = std::min(v.h, int(std::ceil(hy)) + 3);
	if (x1 <= s.x0 || y1 <= s.y0 || x1 - s.x0 > 2 * v.w)
		return false;
	s.w = x1 - s.x0, s.h = y1 - s.y0;
	s.color.assign(size_t(s.w) * s.h, 0);
	s.keep.assign(size_t(s.w) * s.h, 255);
	return true;
}

void Render(Sprite& s, const View& v, Kind k, const Pose& p, const Art& art, int x0, int x1)
{
	if (s.w <= 0)
		return;
	const Target tg{s.color.data(), s.keep.data(), s.w, s.x0, s.y0, s.w, s.h};
	DrawTo(tg, v, k, p, art, x0, x1);
}

void Blit(uint32_t* surf, const View& v, const Sprite& s, int dx, float bright, float alpha, int x0, int x1)
{
	const int c0 = std::max(x0, s.x0 + dx), c1 = std::min(x1, s.x0 + dx + s.w);
	if (c0 >= c1 || alpha <= 0)
		return;
	const int fa = int(std::clamp(alpha, 0.0f, 1.0f) * 256 + 0.5f);
	const int fb = int(std::clamp(bright * alpha, 0.0f, 4.0f) * 256 + 0.5f);
	const int r0 = std::max(0, s.y0), r1 = std::min(v.h, s.y0 + s.h);
	for (int y = r0; y < r1; y++)
	{
		const size_t so = size_t(y - s.y0) * s.w + size_t(c0 - s.x0 - dx);
		const uint32_t* col = s.color.data() + so;
		const uint8_t* kp = s.keep.data() + so;
		uint32_t* d = surf + size_t(y) * v.w + c0;
		for (int i = 0; i < c1 - c0; i++)
		{
			const uint32_t c = col[i];
			const int k = kp[i];
			if (k == 255 && !(c & 0xffffff))
				continue;
			const int km = 255 - (((255 - k) * fa) >> 8); // what stays of the screen, after the fade
			const uint32_t dd = d[i];
			auto ch = [&](int sh) {
				const int v = ((int((dd >> sh) & 0xff) * km * 257 + 32768) >> 16) + ((int((c >> sh) & 0xff) * fb) >> 8);
				return uint32_t(std::min(255, v)) << sh;
			};
			d[i] = 0xff000000u | ch(16) | ch(8) | ch(0);
		}
	}
}
int Wireframe(uint32_t* surf, const View& v, Kind k, const Pose& p, uint32_t front, uint32_t back)
{
	const Mesh& mesh = MeshFor(k);
	const Xform x = Place(p, false);
	const V3 eye{0, v.eye_y, 0};
	auto line = [&](Proj a, Proj b, uint32_t c) {
		const int n = int(std::max(std::fabs(b.sx - a.sx), std::fabs(b.sy - a.sy))) + 1;
		for (int i = 0; i <= n; i++)
		{
			const float t = float(i) / n;
			const int px = int(a.sx + (b.sx - a.sx) * t), py = int(a.sy + (b.sy - a.sy) * t);
			if (px >= 0 && py >= 0 && px < v.w && py < v.h)
				surf[size_t(py) * v.w + px] = c;
		}
	};
	for (int pass = 0; pass < 2; pass++) // the hidden edges first, the visible ones over them
		for (const Tri& t : mesh.t)
		{
			const V3 A = x.Apply(mesh.v[t.a].p), B = x.Apply(mesh.v[t.b].p), C = x.Apply(mesh.v[t.c].p);
			const bool facing = Dot(Cross(C - A, B - A), A - eye) < 0;
			if (facing != (pass == 1) || (!facing && !back))
				continue;
			const Proj a = Project(v, A), b = Project(v, B), c = Project(v, C);
			const uint32_t col = facing ? front : back;
			line(a, b, col), line(b, c, col), line(c, a, col);
		}
	return int(mesh.t.size());
}
} // namespace cart3d
