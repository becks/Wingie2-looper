// Audio looper speed steps: pure-ratio fourths, fifths and octaves. GPL-3.0-or-later.
// Arduino-free so the host tests can include it.
#pragma once
#include <math.h>
namespace wingie_loop {
// -2 oct, -oct, -5th, -4th, unison, +4th, +5th, +oct; within the looper's 0.25-2x range.
constexpr int kSpeedStepCount = 8;
constexpr int kUnisonStep = 4;
constexpr float kSpeedSteps[kSpeedStepCount] = {
  0.25f, 0.5f, 2.0f / 3.0f, 0.75f, 1.0f, 4.0f / 3.0f, 1.5f, 2.0f};
constexpr float kSpeedMin = 0.25f, kSpeedMax = 2.0f;
// Speeds within this of a step count as that step (CC 81 is continuous).
constexpr float kSpeedStepTolerance = 0.002f;

inline const char* speedStepName(int index) {
  static const char* const names[kSpeedStepCount] = {
    "-2oct", "-oct", "-5th", "-4th", "1x", "+4th", "+5th", "+oct"};
  return index >= 0 && index < kSpeedStepCount ? names[index] : "?";
}
// Index of the step the speed sits on, or -1 between steps.
inline int speedStepIndex(float speed) {
  for (int i = 0; i < kSpeedStepCount; ++i)
    if (fabsf(speed - kSpeedSteps[i]) <= kSpeedStepTolerance) return i;
  return -1;
}
// Next step below / above the speed; stays put at the ends of the range.
inline float speedStepDown(float speed) {
  for (int i = kSpeedStepCount - 1; i >= 0; --i)
    if (kSpeedSteps[i] < speed - kSpeedStepTolerance) return kSpeedSteps[i];
  return speed < kSpeedMin ? kSpeedMin : speed;
}
inline float speedStepUp(float speed) {
  for (int i = 0; i < kSpeedStepCount; ++i)
    if (kSpeedSteps[i] > speed + kSpeedStepTolerance) return kSpeedSteps[i];
  return speed > kSpeedMax ? kSpeedMax : speed;
}
// Octave jump from the current speed, clamped to the range.
inline float speedOctave(float speed, bool up) {
  float next = up ? speed * 2 : speed / 2;
  return next < kSpeedMin ? kSpeedMin : next > kSpeedMax ? kSpeedMax : next;
}
// CC 86: 0-127 in eight bands of 16; 64 is unison.
inline int speedStepFromCC(int value) {
  if (value < 0) value = 0;
  if (value > 127) value = 127;
  return value / 16;
}
}  // namespace wingie_loop
