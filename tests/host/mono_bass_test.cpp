// Mono bass (Wingie2/mono_bass.h): the low part of the left/right difference goes,
// the centre and the high part of the stereo image stay.
#include "Wingie2/mono_bass.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace wingie_master;

static const float kRate = 44100.0f;
static const int kBlock = 64;

// Runs a sine on both channels (right = rightScale * left) through `hz` for a
// second, returns the steady state peaks of the output's side and mid.
static void run(float freq, float rightScale, int hz, float& sidePeak, float& midPeak) {
  MonoBass mono;
  float l[kBlock], r[kBlock];
  sidePeak = midPeak = 0;
  const int blocks = (int)kRate / kBlock;
  for (int b = 0; b < blocks; ++b) {
    for (int i = 0; i < kBlock; ++i) {
      const float x = sinf(2.0f * (float)M_PI * freq * (b * kBlock + i) / kRate);
      l[i] = x;
      r[i] = rightScale * x;
    }
    mono.process(l, r, kBlock, hz, kRate);
    if (b < blocks / 2) continue;
    for (int i = 0; i < kBlock; ++i) {
      sidePeak = fmaxf(sidePeak, fabsf(0.5f * (l[i] - r[i])));
      midPeak = fmaxf(midPeak, fabsf(0.5f * (l[i] + r[i])));
    }
  }
}

int main() {
  float side, mid;
  // Hard left 50 Hz with 120 Hz: the side drops to the 2nd order high pass gain
  // (50/120)^2 / sqrt(1 + (50/120)^4) = 0.17 of 0.5; the mid is untouched.
  run(50, 0, 120, side, mid);
  assert(side > 0.07f && side < 0.10f);
  assert(fabsf(mid - 0.5f) < 0.002f);
  // Opposite polarity at 30 Hz, 120 Hz: almost all gone.
  run(30, -1, 120, side, mid);
  assert(side < 0.07f && mid < 0.001f);
  // 2 kHz hard left: stereo kept.
  run(2000, 0, 120, side, mid);
  assert(side > 0.49f && side < 0.51f);
  // A centred signal is unchanged.
  run(50, 1, 120, side, mid);
  assert(side < 1e-6f && fabsf(mid - 1.0f) < 0.002f);
  // Off: the output is the input.
  run(50, 0, 0, side, mid);
  assert(fabsf(side - 0.5f) < 0.002f);

  // Switching on and off crossfades: no step larger than the signal allows.
  MonoBass mono;
  float l[kBlock], r[kBlock], last = 0, maxStep = 0;
  for (int b = 0; b < 200; ++b) {
    const int hz = (b / 50) % 2 ? 300 : 0;
    for (int i = 0; i < kBlock; ++i) {
      l[i] = sinf(2.0f * (float)M_PI * 40 * (b * kBlock + i) / kRate);
      r[i] = 0;
    }
    mono.process(l, r, kBlock, hz, kRate);
    for (int i = 0; i < kBlock; ++i) {
      maxStep = fmaxf(maxStep, fabsf(l[i] - last));
      last = l[i];
    }
  }
  assert(maxStep < 0.02f);  // a 40 Hz sine moves about 0.0057 per sample
  printf("mono_bass_test passed\n");
}
