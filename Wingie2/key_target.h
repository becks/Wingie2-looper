// What the note keys play on each side: the resonator (as always), the audio loop
// (transposed like a sampler) or both. GPL-3.0-or-later.
// Arduino-free so the host tests can include it.
#pragma once
#include <math.h>
#include "speed_steps.h"

namespace wingie_keys {

enum KeyTarget { kResonator = 0, kLoop = 1, kBoth = 2, kKeyTargetCount = 3 };

inline bool playsResonator(int target) { return target != kLoop; }
inline bool playsLoop(int target) { return target == kLoop || target == kBoth; }

inline const char* keyTargetName(int target) {
  static const char* const names[kKeyTargetCount] = {"resonator", "loop", "both"};
  return target >= 0 && target < kKeyTargetCount ? names[target] : "?";
}

// Loop speed for a transposition in semitones from the reference: the panel's C
// with the octave switch in the middle plays the loop as recorded. Equal-tempered,
// within the engine's 0.25-4x (two octaves down, two up).
inline float loopSpeedForSemitones(int semitones) {
  const float speed = powf(2.0f, semitones / 12.0f);
  return speed < wingie_loop::kSpeedMin ? wingie_loop::kSpeedMin
       : speed > wingie_loop::kEngineSpeedMax ? wingie_loop::kEngineSpeedMax : speed;
}

// Panel key 0-11 (C-B) at octave switch -1, 0 or +1.
inline int panelSemitones(int key, int octave) { return key + 12 * octave; }

// A MIDI note on a side: its panel C in the middle octave (MIDI 48 left, 60 right,
// as the resonator's) is the reference.
inline int midiSemitones(int pitch, int referenceNote) { return pitch - referenceNote; }

// CC 95: 0-42 resonator, 43-85 loop, 86-127 both.
inline int keyTargetFromCC(int value) {
  if (value < 0) value = 0;
  if (value > 127) value = 127;
  return value / 43;
}

}  // namespace wingie_keys
