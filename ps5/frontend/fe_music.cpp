// MSX PS5 (2026-10-11, AI-assisted): the shelf's background music - see fe_music.h.
// SPDX-License-Identifier: MIT
#include "fe_music.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace fe
{
namespace music
{
namespace
{
constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979;
constexpr int kHold = -2, kRest = -1;

enum Wave
{
	kSquare,
	kPulse25,
	kTriangle,
	kSine,
	kSaw,
	kBell
};

struct Instr
{
	Wave wave;
	float attack, decay, sustain, release; // seconds, sustain level 0..1
	float gain, pan;                       // pan -1 left .. 1 right
	float vibrato;                         // depth (fraction of the pitch), 5.5 Hz after a moment
	float send;                            // into the echo
};

enum ChordMode
{
	kArp,  // one chord note a step (pattern: '0'..'3' the chord's notes, '.' rest, '-' hold)
	kPad,  // the whole chord held through the bar
	kComp, // the whole chord struck on the pattern's 'x'
	kNone
};

struct TrackDef
{
	const char* name;
	double bpm;
	int steps;             // sixteenths a bar (16, or 12 for 3/4)
	float swing8, swing16; // 0 straight .. 0.33 triplet feel
	int key, scale;        // the key's pitch class (C = 0) and scale (kScales)
	const char* chords;    // one a bar
	const char* const* melody; // explicit, one string a bar ("E5 - A5 . ..."), or null: composed
	int lead_octave;       // the composed melody's middle (MIDI)
	const char* rhythms[4]; // the composed melody's rhythms ('x' a note, '-' hold, '.' rest)
	const char* form;      // a letter a bar: the same letter, the same phrase (on that bar's chord)
	unsigned seed;
	Instr lead, chord, bass;
	ChordMode chord_mode;
	const char* chord_pattern;
	int chord_octave; // the chord's root at or above this MIDI note
	const char* bass_pattern; // R root, O octave, F fifth, T third, W walking, A approach the next bar, - hold, . rest
	int bass_octave;
	const char *kick, *snare, *hat, *ride, *rim, *shaker; // 'x' hit, 'o' soft, '.' nothing
	float drum_gain;
	float echo_steps, echo_feedback; // the echo's time (sixteenths) and feedback
	float crackle;                   // a record's crackle
	float tone;                      // the mix's low-pass, 0..1 (1: open)
};

const int kScales[7][8] = {
	{0, 2, 4, 5, 7, 9, 11, -1}, // major
	{0, 2, 3, 5, 7, 8, 10, -1}, // minor
	{0, 2, 4, 7, 9, -1},        // major pentatonic
	{0, 3, 5, 7, 10, -1},       // minor pentatonic
	{0, 2, 3, 5, 7, 9, 10, -1}, // dorian
	{0, 2, 4, 5, 7, 9, 10, -1}, // mixolydian
	{0, 3, 5, 6, 7, 10, -1},    // blues
};
enum
{
	kMajor,
	kMinor,
	kMajorPenta,
	kMinorPenta,
	kDorian,
	kMixolydian,
	kBlues
};

// ---- the tracks ------------------------------------------------------------------------------------------------
const char* const kClassicMelody[] = {
	"E5 - A5 - C6 - B5 - A5 - - - E5 - G5 -", "F5 - A5 - C6 - A5 - G5 - F5 - E5 - - -",
	"E5 - G5 - C6 - D6 - E6 - D6 - C6 - G5 -", "D6 - - - B5 - G5 - A5 - B5 - D6 - - -",
	"C6 - B5 - A5 - E5 - A5 - C6 - E6 - - -", "D6 - C6 - A5 - F5 - A5 - C6 - F6 - - -",
	"E6 - D6 - B5 - G5 - D6 - B5 - G5 - B5 -", "G#5 - - - B5 - - - E6 - - - . . . .",
	"F5 - E5 - D5 - A5 - - - F5 - A5 - D6 -", "B5 - - - G5 - D5 - G5 - B5 - D6 - - -",
	"C6 - - - G5 - E5 - G5 - C6 - E6 - D6 -", "C6 - B5 - A5 - - - E5 - - - A5 - - -",
	"D6 - C6 - A5 - F5 - D5 - F5 - A5 - D6 -", "B5 - - - G#5 - - - E5 - G#5 - B5 - - -",
	"A5 - - - E5 - A5 - C6 - B5 - A5 - - -", "A5 - - - - - - - . . . . . . . .",
};

const TrackDef kTracks[] = {
	{"MSX Classic", 132, 16, 0, 0, 9, kMinor, "Am F C G Am F G E Dm G C Am Dm E Am Am", kClassicMelody, 0, {}, "", 1,
		{kSquare, 0.004f, 0.12f, 0.6f, 0.03f, 0.50f, 0.1f, 0.003f, 0.0f},
		{kSquare, 0.002f, 0.05f, 0.3f, 0.02f, 0.22f, -0.15f, 0, 0.0f},
		{kSquare, 0.002f, 0.08f, 0.55f, 0.02f, 0.40f, 0, 0, 0.0f}, kArp, "0123012301230123", 69, "R-O-R-O-R-O-R-O-",
		33, "x.......x.......", "....x.......x...", "..x...x...x...x.", nullptr, nullptr, nullptr, 1.0f, 3, 0, 0, 0.8f},
	{"Ambient Drift", 68, 16, 0, 0, 2, kMajorPenta,
		"Dmaj7 Dmaj7 Bm7 Bm7 Gmaj7 Gmaj7 A6 A6 Dmaj7 Dmaj7 Bm7 Bm7 Gmaj7 Gmaj7 Em7 A6", nullptr, 76,
		{"x.......x.......", "....x.......x...", "x...........x...", "x..............."}, "ABACABADABACABAE", 7,
		{kBell, 0.005f, 2.5f, 0.0f, 1.5f, 0.36f, 0.35f, 0, 0.6f},
		{kSine, 1.4f, 1.0f, 0.8f, 2.0f, 0.16f, -0.25f, 0.002f, 0.45f},
		{kSine, 0.4f, 1.0f, 0.8f, 1.5f, 0.30f, 0, 0, 0.1f}, kPad, "", 57, "R---------------", 38, nullptr, nullptr,
		nullptr, nullptr, nullptr, nullptr, 0, 6, 0.55f, 0, 0.35f},
	{"Lo-fi Chill", 78, 16, 0.18f, 0, 5, kMajorPenta,
		"Gm7 C7 Fmaj7 Dm7 Bbmaj7 Am7 Gm7 C7 Gm7 C7 Fmaj7 Dm7 Bbmaj7 Am7 Gm7 C7", nullptr, 72,
		{"x.....x...x.....", "..x...x.....x...", "x.......x.x.....", "....x...x.....x."}, "ABACABADABACABAE", 11,
		{kTriangle, 0.01f, 0.3f, 0.5f, 0.2f, 0.32f, 0.25f, 0.003f, 0.25f},
		{kTriangle, 0.01f, 0.6f, 0.35f, 0.3f, 0.20f, -0.2f, 0, 0.15f},
		{kSine, 0.01f, 0.3f, 0.7f, 0.1f, 0.42f, 0, 0, 0}, kComp, "x.....x...x.....", 57, "R.....R...F...R.", 41,
		"x......x..x.....", "....x.......x...", "o.o.o.o.o.o.o.o.", nullptr, nullptr, nullptr, 0.8f, 3, 0.3f, 1.0f, 0.35f},
	{"Boom Bap", 90, 16, 0, 0.10f, 0, kMinorPenta,
		"Cm7 Cm7 Abmaj7 G7 Cm7 Cm7 Abmaj7 G7 Cm7 Cm7 Abmaj7 G7 Cm7 Cm7 Abmaj7 G7", nullptr, 72,
		{"x..x..x...x.....", "x..x..x...x..x..", "x.....x.x...x...", "x..x....x..x...."}, "AABAAABCAABAAABD", 23,
		{kPulse25, 0.005f, 0.15f, 0.4f, 0.08f, 0.28f, 0.2f, 0, 0.1f},
		{kTriangle, 0.005f, 0.25f, 0.2f, 0.1f, 0.22f, -0.2f, 0, 0.05f},
		{kSine, 0.005f, 0.4f, 0.6f, 0.1f, 0.55f, 0, 0, 0}, kComp, "x..x......x.....", 60, "R.....R...R.R...", 36,
		"x.....x.x.......", "....x.......x...", "x.x.x.x.x.xxx.x.", nullptr, nullptr, nullptr, 1.2f, 3, 0.2f, 0.5f, 0.5f},
	{"Late Night Jazz", 128, 16, 0.30f, 0, 0, kMajor,
		"Dm7 G7 Cmaj7 A7 Dm7 G7 Em7 A7 Fmaj7 Fm6 Em7 A7 Dm7 G7 Cmaj7 Cmaj7", nullptr, 72,
		{"x.x.x...x.x.x...", "x...x.x.x...x.x.", "..x.x.x.x...x...", "x.x...x.x.x.x..."}, "ABCDEFGHABCDIJKL", 31,
		{kBell, 0.003f, 0.8f, 0.12f, 0.4f, 0.34f, 0.25f, 0, 0.2f},
		{kTriangle, 0.01f, 0.4f, 0.3f, 0.15f, 0.18f, -0.25f, 0, 0.1f},
		{kTriangle, 0.005f, 0.2f, 0.6f, 0.05f, 0.45f, 0, 0, 0}, kComp, "...x......x.....", 57, "R...W...F...A...", 36,
		nullptr, nullptr, "....o.......o...", "x...x.x.x...x.x.", nullptr, nullptr, 0.7f, 3, 0.15f, 0, 0.5f},
	{"Synthwave", 108, 16, 0, 0, 9, kMinor, "Am Am F F C C G G Am Am F F C C G E", nullptr, 69,
		{"x...x...x.x.x...", "x.......x...x.x.", "x.x.x...x...x...", "x...........x..."}, "AABAAABCAABAAABD", 41,
		{kSaw, 0.01f, 0.3f, 0.6f, 0.15f, 0.24f, 0.2f, 0.004f, 0.35f},
		{kSaw, 0.4f, 0.5f, 0.7f, 0.6f, 0.11f, -0.3f, 0.002f, 0.3f},
		{kSaw, 0.003f, 0.1f, 0.5f, 0.05f, 0.34f, 0, 0, 0}, kPad, "", 57, "R.R.R.R.R.R.R.R.", 33, "x...x...x...x...",
		"....x.......x...", "..x...x...x...x.", nullptr, nullptr, nullptr, 1.0f, 3, 0.45f, 0, 0.6f},
	{"Bossa Breeze", 120, 16, 0, 0, 7, kMajor,
		"Gmaj7 Gmaj7 Am7 D7 Bm7 E7 Am7 D7 Gmaj7 Gmaj7 Am7 D7 Bm7 E7 Am7 D7", nullptr, 72,
		{"x.....x...x.....", "x.x...x.....x...", "....x.x.x.......", "x.......x.x.x..."}, "ABACABADABACABAE", 53,
		{kSine, 0.04f, 0.3f, 0.7f, 0.15f, 0.34f, 0.25f, 0.005f, 0.2f},
		{kTriangle, 0.003f, 0.3f, 0.15f, 0.1f, 0.20f, -0.25f, 0, 0.1f},
		{kSine, 0.005f, 0.25f, 0.6f, 0.1f, 0.42f, 0, 0, 0}, kComp, "x..x..x...x..x..", 55, "R.....F.R.....F.", 31,
		"o.....o.o.....o.", nullptr, nullptr, nullptr, "x..x..x...x.x...", "oooooooooooooooo", 0.6f, 3, 0.2f, 0, 0.45f},
	{"Turbo Arcade", 168, 16, 0, 0, 4, kMinor, "Em C D Bm Em C D B Em C D Bm C D B B", nullptr, 76,
		{"x.x.x.x.x.x.x.x.", "x.xxx.x.x.xxx.x.", "x...x.x.x.x.x...", "xxxxx.x.x.x.x.x."}, "AABAAABCAABAAABD", 61,
		{kSquare, 0.002f, 0.08f, 0.5f, 0.02f, 0.36f, 0.1f, 0.002f, 0.1f},
		{kPulse25, 0.001f, 0.04f, 0.3f, 0.01f, 0.16f, -0.2f, 0, 0},
		{kSquare, 0.001f, 0.06f, 0.5f, 0.02f, 0.32f, 0, 0, 0}, kArp, "0123012301230123", 64, "R.O.R.O.R.O.R.O.", 28,
		"x...x...x...x...", "....x.......x..x", "oooooooooooooooo", nullptr, nullptr, nullptr, 0.9f, 3, 0.2f, 0, 0.8f},
	{"Funky Groove", 104, 16, 0, 0.08f, 4, kBlues, "E9 E9 A9 A9 E9 E9 B7 A7 E9 E9 A9 A9 E9 E9 B7 A7", nullptr, 71,
		{"x..x..x.x.....x.", "..x.x...x..x....", "x.x...x...x.x...", "x......x.x.x.x.."}, "ABACABADABACABAE", 71,
		{kSquare, 0.004f, 0.12f, 0.3f, 0.05f, 0.30f, 0.2f, 0, 0.1f},
		{kPulse25, 0.002f, 0.1f, 0.1f, 0.05f, 0.20f, -0.25f, 0, 0},
		{kSaw, 0.002f, 0.12f, 0.3f, 0.04f, 0.45f, 0, 0, 0}, kComp, "..x..x....x..x..", 59, "R..R..O.R.R.F..O", 28,
		"x..x......x.x...", "....x..o....x..o", "x.xxx.xxx.xxx.x.", nullptr, nullptr, nullptr, 1.0f, 3, 0.15f, 0, 0.55f},
	{"Music Box", 84, 12, 0, 0, 0, kMajorPenta, "C Am F G C Em F G C Am F G Am F G C", nullptr, 79,
		{"x.x.x.x.x.x.", "x...x.x.x...", "x.x.x...x...", "x.....x.x.x."}, "ABACABADABACABAE", 83,
		{kBell, 0.002f, 1.4f, 0.0f, 1.0f, 0.40f, 0.2f, 0, 0.3f},
		{kBell, 0.002f, 0.9f, 0.0f, 0.6f, 0.16f, -0.25f, 0, 0.2f},
		{kSine, 0.01f, 0.8f, 0.3f, 0.5f, 0.28f, 0, 0, 0}, kArp, "0...1...2...", 60, "R...........", 36, nullptr,
		nullptr, nullptr, nullptr, nullptr, nullptr, 0, 4, 0.35f, 0, 0.5f},
};
constexpr int kTrackCount = int(sizeof(kTracks) / sizeof(kTracks[0]));

// ---- notes and chords --------------------------------------------------------------------------------------------
double Freq(int midi)
{
	return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

int PitchClass(const char*& p)
{
	static const int base[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
	int n = base[*p++ - 'A'];
	if (*p == '#')
		n++, p++;
	else if (*p == 'b')
		n--, p++;
	return (n + 12) % 12;
}

int ParseNote(const std::string& t)
{
	if (t == "-")
		return kHold;
	if (t.empty() || t == ".")
		return kRest;
	const char* p = t.c_str();
	const int pc = PitchClass(p);
	return pc + 12 * ((*p - '0') + 1);
}

struct Chord
{
	int root = 0;            // pitch class
	std::vector<int> notes;  // intervals from the root
};

Chord ParseChord(const std::string& t)
{
	Chord c;
	const char* p = t.c_str();
	c.root = PitchClass(p);
	const std::string q = p;
	if (q == "m")
		c.notes = {0, 3, 7};
	else if (q == "7")
		c.notes = {0, 4, 7, 10};
	else if (q == "m7")
		c.notes = {0, 3, 7, 10};
	else if (q == "maj7")
		c.notes = {0, 4, 7, 11};
	else if (q == "6")
		c.notes = {0, 4, 7, 9};
	else if (q == "m6")
		c.notes = {0, 3, 7, 9};
	else if (q == "9")
		c.notes = {0, 4, 10, 14};
	else if (q == "dim")
		c.notes = {0, 3, 6, 9};
	else if (q == "sus4")
		c.notes = {0, 5, 7};
	else
		c.notes = {0, 4, 7};
	return c;
}

std::vector<std::string> Words(const char* s)
{
	std::vector<std::string> w;
	std::string cur;
	for (; s && *s; s++)
		if (*s == ' ')
		{
			if (!cur.empty())
				w.push_back(cur), cur.clear();
		}
		else
			cur += *s;
	if (!cur.empty())
		w.push_back(cur);
	return w;
}

char PatternAt(const char* p, int s)
{
	if (!p)
		return '.';
	const int n = int(strlen(p));
	return s < n ? p[s] : '.';
}

struct Rng
{
	uint32_t s;
	explicit Rng(uint32_t seed) : s(seed * 2654435761u + 12345u) {}
	uint32_t Next()
	{
		s ^= s << 13, s ^= s >> 17, s ^= s << 5;
		return s;
	}
	int Pick(int n) { return int(Next() % uint32_t(n)); }
};

// ---- the voices ----------------------------------------------------------------------------------------------------
struct Voice
{
	const Instr* in = nullptr;
	double phase = 0, freq = 0, age = 0;
	float level = 0;
	int stage = 4; // 0 attack, 1 decay, 2 sustain, 3 release, 4 off

	void On(double f)
	{
		freq = f, age = 0, stage = 0;
		if (in && in->wave == kBell)
			phase = 0;
	}
	void Off()
	{
		if (stage < 3)
			stage = 3;
	}
	float Sample(double dt)
	{
		if (stage == 4 || !in)
			return 0;
		const Instr& i = *in;
		switch (stage)
		{
			case 0:
				level += i.attack > 0 ? float(dt / i.attack) : 1.0f;
				if (level >= 1)
					level = 1, stage = 1;
				break;
			case 1:
				level -= float((1 - i.sustain) * dt / std::max(0.001f, i.decay));
				if (level <= i.sustain)
					level = i.sustain, stage = 2;
				if (i.sustain <= 0.001f && level <= 0.0005f)
					stage = 4;
				break;
			case 3:
				level *= float(std::exp(-dt / std::max(0.002f, i.release)));
				if (level < 0.0005f)
					level = 0, stage = 4;
				break;
		}
		const double vib = (i.vibrato > 0 && age > 0.18) ? 1.0 + i.vibrato * std::sin(age * 2 * kPi * 5.5) : 1.0;
		phase += freq * vib * dt;
		phase -= std::floor(phase);
		age += dt;
		float w;
		switch (i.wave)
		{
			case kSquare: w = phase < 0.5 ? 0.8f : -0.8f; break;
			case kPulse25: w = phase < 0.25 ? 0.8f : -0.8f; break;
			case kTriangle: w = float(phase < 0.5 ? 4 * phase - 1 : 3 - 4 * phase); break;
			case kSine: w = float(std::sin(2 * kPi * phase)); break;
			case kSaw: w = float(2 * phase - 1) * 0.7f; break;
			default: // a bell: a sine and a bright partial that dies away first
				w = float(std::sin(2 * kPi * phase) + 0.45 * std::exp(-age / 0.25) * std::sin(2 * kPi * phase * 2.76));
				break;
		}
		return w * level * i.gain;
	}
};

// ---- a track playing ---------------------------------------------------------------------------------------------
struct Player
{
	const TrackDef* t = nullptr;
	int bars = 1;
	std::vector<Chord> chords;
	std::vector<int> melody; // a step each: MIDI, kHold or kRest
	Voice lead, bass, chord[4];
	int step = -1;
	double into_step = 1e9;
	double kick = 9, snare = 9, hat = 9, ride = 9, rim = 9, shaker = 9;
	float kick_v = 0, snare_v = 0, hat_v = 0, ride_v = 0, rim_v = 0, shaker_v = 0;
	uint32_t lfsr = 1;
	float hp_l = 0, ride_lp = 0;
	std::vector<float> echo_l, echo_r;
	size_t echo_pos = 0, echo_len = 1;
	float lp_l = 0, lp_r = 0, norm = 1;
	Rng crackle_rng{99};

	explicit Player(int track)
	{
		t = &kTracks[std::clamp(track, 0, kTrackCount - 1)];
		for (const std::string& w : Words(t->chords))
			chords.push_back(ParseChord(w));
		bars = std::max<int>(1, int(chords.size()));
		lead.in = &t->lead, bass.in = &t->bass;
		for (Voice& v : chord)
			v.in = &t->chord;
		Compose();
		const double step_s = 60.0 / t->bpm / 4.0;
		echo_len = size_t(std::max(1.0, t->echo_steps * step_s * kRate));
		echo_l.assign(echo_len, 0), echo_r.assign(echo_len, 0);
	}

	int ScaleNear(int midi, int delta) const
	{
		// the scale's note `delta` scale steps from the one nearest `midi`
		std::vector<int> sc;
		for (int i = 0; i < 8 && kScales[t->scale][i] >= 0; i++)
			sc.push_back(kScales[t->scale][i]);
		std::vector<int> notes;
		for (int o = 2; o < 9; o++)
			for (int d : sc)
				notes.push_back(t->key + d + 12 * o);
		size_t best = 0;
		for (size_t i = 0; i < notes.size(); i++)
			if (std::abs(notes[i] - midi) < std::abs(notes[best] - midi))
				best = i;
		const int j = std::clamp(int(best) + delta, 0, int(notes.size()) - 1);
		return notes[size_t(j)];
	}
	int ChordToneNear(const Chord& c, int midi) const
	{
		int best = midi, bd = 99;
		for (int o = -2; o <= 2; o++)
			for (int iv : c.notes)
			{
				const int n = (midi / 12 + o) * 12 + (c.root + iv) % 12;
				if (std::abs(n - midi) < bd)
					bd = std::abs(n - midi), best = n;
			}
		return best;
	}

	void Compose()
	{
		const int steps = t->steps;
		melody.assign(size_t(bars * steps), kRest);
		if (t->melody)
		{
			for (int b = 0; b < bars; b++)
			{
				const std::vector<std::string> w = Words(t->melody[b]);
				for (int s = 0; s < steps; s++)
					melody[size_t(b * steps + s)] = s < int(w.size()) ? ParseNote(w[size_t(s)]) : kRest;
			}
			return;
		}
		// a phrase a letter: its rhythm and its moves are the letter's, its notes fit the bar's chord
		const int form_len = std::max<int>(1, int(strlen(t->form)));
		int prev = t->lead_octave;
		for (int b = 0; b < bars; b++)
		{
			const char letter = t->form[b % form_len];
			Rng r(t->seed * 131u + uint32_t(letter) * 7919u);
			const char* rhythm = t->rhythms[r.Pick(4)];
			const Chord& c = chords[size_t(b)];
			int note = ChordToneNear(c, t->lead_octave + (r.Pick(3) - 1) * 4);
			bool first = true;
			int last_x = -1;
			for (int s = 0; s < steps; s++)
			{
				const char ch = PatternAt(rhythm, s);
				if (ch == 'x')
				{
					if (!first)
					{
						static const int moves[8] = {-2, -1, -1, 1, 1, 2, -3, 3};
						note = ScaleNear(note, moves[r.Pick(8)]);
						if (s % 4 == 0)
							note = ChordToneNear(c, note);
						if (note > t->lead_octave + 12)
							note = ScaleNear(note, -3);
						if (note < t->lead_octave - 7)
							note = ScaleNear(note, 3);
					}
					first = false;
					melody[size_t(b * steps + s)] = note;
					last_x = s;
				}
				else
					melody[size_t(b * steps + s)] = ch == '-' ? kHold : kRest;
			}
			// the end of each four bars comes home to the chord's root
			if (b % 4 == 3 && last_x >= 0)
				melody[size_t(b * steps + last_x)] = ChordToneNear(Chord{c.root, {0}}, melody[size_t(b * steps + last_x)]);
			(void)prev;
		}
	}

	double StepLength(int s) const
	{
		const double base = kRate * 60.0 / t->bpm / 4.0;
		const double s16 = 1 + t->swing16 * ((s % 2 == 0) ? 1 : -1);
		const double s8 = 1 + t->swing8 * ((s % 4) < 2 ? 1 : -1);
		return base * s16 * s8;
	}

	int ChordNote(const Chord& c, int idx) const
	{
		int root = t->chord_octave - ((t->chord_octave % 12 - c.root + 12) % 12) + 12;
		while (root - 12 >= t->chord_octave)
			root -= 12;
		while (root < t->chord_octave)
			root += 12;
		const int n = int(c.notes.size());
		return root + c.notes[size_t(idx % n)] + 12 * (idx / n);
	}

	void NextStep()
	{
		const int steps = t->steps;
		step = (step + 1) % (bars * steps);
		const int b = step / steps, s = step % steps;
		const Chord& c = chords[size_t(b)];
		const Chord& next = chords[size_t((b + 1) % bars)];
		// the melody
		const int m = melody[size_t(step)];
		if (m >= 0)
			lead.On(Freq(m));
		else if (m == kRest)
			lead.Off();
		// the chord part
		const char cp = PatternAt(t->chord_pattern, s);
		switch (t->chord_mode)
		{
			case kArp:
				if (cp >= '0' && cp <= '9')
					chord[0].On(Freq(ChordNote(c, cp - '0')));
				else if (cp == '.')
					chord[0].Off();
				break;
			case kPad:
				if (s == 0)
					for (int i = 0; i < 4; i++)
					{
						if (i < int(c.notes.size()))
							chord[i].On(Freq(ChordNote(c, i)));
						else
							chord[i].Off();
					}
				break;
			case kComp:
				if (cp == 'x')
					for (int i = 0; i < 4; i++)
					{
						if (i < int(c.notes.size()))
							chord[i].On(Freq(ChordNote(c, i)));
					}
				else if (cp == '.')
					for (Voice& v : chord)
						v.Off();
				break;
			default: break;
		}
		// the bass
		const char bp = PatternAt(t->bass_pattern, s);
		int root = t->bass_octave + ((c.root - t->bass_octave % 12) + 12) % 12;
		int bn = -1;
		switch (bp)
		{
			case 'R': bn = root; break;
			case 'O': bn = root + 12; break;
			case 'F': bn = root + 7; break;
			case 'T': bn = root + c.notes[1 % c.notes.size()]; break;
			case 'W': bn = root + c.notes[size_t((s / 4) % c.notes.size())] + ((s / 4) % 2 ? 0 : 12) - 12; break;
			case 'A':
			{
				const int nr = t->bass_octave + ((next.root - t->bass_octave % 12) + 12) % 12;
				bn = nr + ((b % 2) ? 1 : -1);
				break;
			}
			case '.': bass.Off(); break;
			default: break;
		}
		if (bn >= 0)
			bass.On(Freq(bn));
		// drums
		auto hit = [&](const char* p, double& age, float& v) {
			const char ch = PatternAt(p, s);
			if (ch == 'x' || ch == 'o')
				age = 0, v = ch == 'x' ? 1.0f : 0.45f;
		};
		hit(t->kick, kick, kick_v);
		hit(t->snare, snare, snare_v);
		hit(t->hat, hat, hat_v);
		hit(t->ride, ride, ride_v);
		hit(t->rim, rim, rim_v);
		hit(t->shaker, shaker, shaker_v);
	}

	void Frame(float& out_l, float& out_r)
	{
		if (into_step >= StepLength(step < 0 ? 0 : step % t->steps))
		{
			into_step = step < 0 ? 0 : into_step - StepLength(step % t->steps);
			NextStep();
		}
		into_step += 1;
		const double dt = 1.0 / kRate;
		float l = 0, r = 0, send = 0;
		auto add = [&](Voice& v) {
			const float x = v.Sample(dt);
			if (x == 0 || !v.in)
				return;
			l += x * (1 - std::max(0.0f, v.in->pan)), r += x * (1 + std::min(0.0f, v.in->pan));
			send += x * v.in->send;
		};
		add(lead);
		add(bass);
		for (Voice& v : chord)
			add(v);
		// noise (the PSG's 17 bits) for the drums
		if ((lfsr & 1) ^ ((lfsr >> 3) & 1))
			lfsr = (lfsr >> 1) | 0x10000u;
		else
			lfsr >>= 1;
		const float noise = (lfsr & 1) ? 1.0f : -1.0f;
		hp_l += (noise - hp_l) * 0.35f;
		const float bright = noise - hp_l; // the noise's top
		float drums = 0;
		const float g = t->drum_gain;
		if (kick < 0.14)
		{
			const double f = 48 + 95 * std::exp(-kick / 0.025);
			drums += 0.75f * kick_v * float(std::exp(-kick / 0.06) * std::sin(2 * kPi * f * kick));
			kick += dt;
		}
		if (snare < 0.18)
		{
			drums += 0.38f * snare_v * float(std::exp(-snare / 0.055)) * noise +
				0.18f * snare_v * float(std::exp(-snare / 0.03) * std::sin(2 * kPi * 185 * snare));
			snare += dt;
		}
		if (hat < 0.05)
		{
			drums += 0.20f * hat_v * float(std::exp(-hat / 0.012)) * bright;
			hat += dt;
		}
		if (ride < 0.6)
		{
			ride_lp += (bright - ride_lp) * 0.6f;
			drums += 0.13f * ride_v * float(std::exp(-ride / 0.22)) * ride_lp;
			ride += dt;
		}
		if (rim < 0.03)
		{
			drums += 0.30f * rim_v * float(std::exp(-rim / 0.006) * std::sin(2 * kPi * 1700 * rim));
			rim += dt;
		}
		if (shaker < 0.05)
		{
			drums += 0.08f * shaker_v * float(std::sin(kPi * shaker / 0.05)) * bright;
			shaker += dt;
		}
		l += drums * g, r += drums * g;
		// a record's crackle
		if (t->crackle > 0)
		{
			const uint32_t k = crackle_rng.Next();
			const float c = (k % 6000u == 0 ? 0.25f : 0.0f) + ((int(k >> 8) & 1023) - 512) / 512.0f * 0.006f;
			l += c * t->crackle, r += c * t->crackle;
		}
		// the echo (ping-pong)
		if (t->echo_feedback > 0)
		{
			const float el = echo_l[echo_pos], er = echo_r[echo_pos];
			echo_l[echo_pos] = send + er * t->echo_feedback;
			echo_r[echo_pos] = el * t->echo_feedback;
			echo_pos = (echo_pos + 1) % echo_len;
			l += el * 0.6f, r += er * 0.6f;
		}
		// the tone: a one-pole low-pass
		const float k = 0.08f + 0.85f * t->tone;
		lp_l += (l - lp_l) * k, lp_r += (r - lp_r) * k;
		out_l = lp_l * norm, out_r = lp_r * norm;
	}

	// Evens the tracks' loudness: plays the first seconds silently and scales to a common level.
	void Normalize()
	{
		Player probe(int(t - kTracks));
		double sum = 0;
		const int n = kRate * 6;
		for (int i = 0; i < n; i++)
		{
			float a, b;
			probe.Frame(a, b);
			sum += 0.5 * (double(a) * a + double(b) * b);
		}
		const double rms = std::sqrt(sum / n);
		norm = rms > 1e-4 ? float(std::clamp(0.12 / rms, 0.2, 6.0)) : 1.0f;
	}
};

std::unique_ptr<Player> g_player;
// each track's loudness scale, measured on the PC (tests/music_norms.cpp): measuring takes the console a moment
const float kNormTable[] = {0.3320f, 0.4666f, 0.5805f, 0.4477f, 0.5348f, 0.7135f, 0.5458f, 0.4462f, 0.6385f, 0.5071f};
static_assert(sizeof(kNormTable) / sizeof(kNormTable[0]) == size_t(kTrackCount), "a scale for each track");
} // namespace

int TrackCount()
{
	return kTrackCount;
}

const char* TrackName(int track)
{
	return kTracks[std::clamp(track, 0, kTrackCount - 1)].name;
}

void Select(int track)
{
	track = std::clamp(track, 0, kTrackCount - 1);
	g_player.reset(new Player(track));
	g_player->norm = kNormTable[track];
}

void Render(float* l, float* r, int frames)
{
	if (!g_player)
		return;
	for (int i = 0; i < frames; i++)
	{
		float a, b;
		g_player->Frame(a, b);
		l[i] += a, r[i] += b;
	}
}

float MeasureNorm(int track)
{
	Player p(track);
	p.Normalize();
	return p.norm;
}

void RenderTrack(int track, int16_t* stereo, int frames, float gain)
{
	Player p(track);
	p.Normalize();
	for (int i = 0; i < frames; i++)
	{
		float a, b;
		p.Frame(a, b);
		stereo[i * 2] = int16_t(std::clamp(a * gain, -1.0f, 1.0f) * 32767);
		stereo[i * 2 + 1] = int16_t(std::clamp(b * gain, -1.0f, 1.0f) * 32767);
	}
}
} // namespace music
} // namespace fe
