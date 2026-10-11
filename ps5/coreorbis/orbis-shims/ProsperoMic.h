// MSX PS5: the controller's microphone (libSceAudioIn, loaded at run time: the SDK has no stub for it), listened to
// on a thread of its own - is it on (sound coming in; muted gives silence), and is someone blowing into it.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace ps5mic
{
// Starts listening to the user's microphone; false if it can't be opened (boot.log says why).
bool Start(int32_t user);
void Stop();

// Sound is coming in: the microphone is on (the mute button's light off).
bool Live();
// 0..1: someone blowing into it (loud, steady, rumbling).
float Blow();
// A port is open (the system's microphone library answered).
bool Available();
// The system has the microphone unmuted (the mute button's light off), whether or not sound comes in.
bool Unmuted();
} // namespace ps5mic
