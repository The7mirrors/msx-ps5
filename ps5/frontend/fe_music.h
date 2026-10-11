// MSX PS5 (2026-10-11, AI-assisted): the shelf's background music - ten original tracks in different styles, played
// live by a small synth (square / pulse / triangle / sine / saw / bell voices, drum sounds, swing, an echo). Nothing is
// loaded: each track is its chords, tempo, patterns and instruments, and its melody is composed from the chords.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace fe
{
namespace music
{
int TrackCount();
const char* TrackName(int track); // 0-based

// The shelf's player: Select starts a track from its beginning (its loudness is evened out with the others);
// Render adds `frames` stereo frames (about -0.5..0.5) to l / r.
void Select(int track);
void Render(float* l, float* r, int frames);

// The scale that evens a track's loudness (measured by playing its first seconds; slow - the shelf uses the
// numbers measured on the PC, kNormTable in fe_music.cpp).
float MeasureNorm(int track);

// A track on its own (a preview on the PC): 16-bit stereo, `gain` applied.
void RenderTrack(int track, int16_t* stereo, int frames, float gain);
} // namespace music
} // namespace fe
