#include "Wingie2/side_filter.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

using wingie_loop::SideFilter;

static const float kRate = 44100;

// RMS of a sine at `freq` through the filter, after it has settled.
static float response(int type, float cutoff, float resonance, float freq) {
  SideFilter f;
  std::vector<float> block(64);
  double sum = 0;
  int count = 0;
  for (int b = 0; b < 2000; ++b) {
    for (int i = 0; i < 64; ++i) block[i] = sinf(2 * (float)M_PI * freq * (b * 64 + i) / kRate);
    f.process(block.data(), 64, type, cutoff, resonance, kRate);
    if (b >= 1000)
      for (float x : block) { sum += x * x; ++count; }
  }
  return (float)std::sqrt(sum / count) * std::sqrt(2.0f);
}

int main() {
  // Off passes the signal untouched.
  {
    SideFilter f;
    float block[64];
    for (int i = 0; i < 64; ++i) block[i] = 0.01f * i - 0.3f;
    float copy[64];
    std::copy(block, block + 64, copy);
    f.process(block, 64, wingie_loop::kFilterOff, 1000, 0.5f, kRate);
    for (int i = 0; i < 64; ++i) assert(block[i] == copy[i]);
  }
  // Low pass: passes well below the cutoff, cuts well above (12 dB/octave).
  assert(response(1, 1000, 0, 100) > 0.95f);
  assert(response(1, 1000, 0, 8000) < 0.03f);
  // High pass: the mirror image.
  assert(response(2, 1000, 0, 8000) > 0.95f);
  assert(response(2, 1000, 0, 100) < 0.03f);
  // Band pass: unity at the centre, down on both sides; resonance narrows it.
  assert(std::fabs(response(3, 1000, 0, 1000) - 1) < 0.05f);
  // Band pass at the lowest resonance is already Q 1: an octave away ~-5 dB (0.555).
  assert(response(3, 1000, 0, 100) < 0.12f && response(3, 1000, 0, 10000) < 0.12f);
  assert(std::fabs(response(3, 1000, 0, 2000) - 0.555f) < 0.03f);
  assert(response(3, 1000, 1, 2000) < 0.1f);
  assert(response(3, 1000, 1, 2000) < response(3, 1000, 0, 2000));
  // Resonance lifts the low pass at the cutoff, but stays bounded.
  const float peak = response(1, 1000, 1, 1000);
  assert(peak > 3 && peak < 8);
  // Extreme cutoffs and full resonance stay finite.
  for (float cutoff : {1.0f, 20.0f, 16000.0f, 30000.0f})
    for (int type = 1; type <= 3; ++type) assert(std::isfinite(response(type, cutoff, 1, 440)));
  // A non-finite input does not leave the filter stuck.
  {
    SideFilter f;
    float block[64];
    std::fill(block, block + 64, NAN);
    f.process(block, 64, 1, 1000, 0.5f, kRate);
    std::fill(block, block + 64, 0.25f);
    for (int b = 0; b < 200; ++b) {
      std::fill(block, block + 64, 0.25f);
      f.process(block, 64, 1, 1000, 0.5f, kRate);
    }
    assert(std::fabs(block[63] - 0.25f) < 0.01f);
  }
  // Switching type or turning it on/off crossfades: no large step.
  {
    SideFilter f;
    float block[64];
    float previous = 0, biggest = 0;
    int types[] = {0, 1, 1, 2, 2, 3, 3, 0, 0, 2, 0};
    int n = 0;
    for (int type : types) {
      for (int b = 0; b < 20; ++b) {
        for (int i = 0; i < 64; ++i, ++n) block[i] = 0.5f * sinf(2 * (float)M_PI * 300 * n / kRate);
        f.process(block, 64, type, 600, 0.3f, kRate);
        for (int i = 0; i < 64; ++i) {
          biggest = std::max(biggest, std::fabs(block[i] - previous));
          previous = block[i];
        }
      }
    }
    // A 300 Hz sine at 0.5 moves at most ~0.021 per sample.
    assert(biggest < 0.06f);
  }
  // Cutoff jumps glide: a sweep does not blow up and settles.
  {
    SideFilter f;
    float block[64];
    for (int b = 0; b < 400; ++b) {
      for (int i = 0; i < 64; ++i) block[i] = (i % 2) ? 0.3f : -0.3f;
      f.process(block, 64, 1, (b / 20) % 2 ? 16000 : 20, 1, kRate);
      for (float x : block) assert(std::isfinite(x) && std::fabs(x) < 4);
    }
  }
  // CC mappings.
  assert(wingie_loop::filterTypeFromCC(0) == 0 && wingie_loop::filterTypeFromCC(31) == 0);
  assert(wingie_loop::filterTypeFromCC(32) == 1 && wingie_loop::filterTypeFromCC(64) == 2);
  assert(wingie_loop::filterTypeFromCC(96) == 3 && wingie_loop::filterTypeFromCC(127) == 3);
  assert(std::fabs(wingie_loop::filterCutoffFromCC(0) - 20) < 0.01f);
  assert(std::fabs(wingie_loop::filterCutoffFromCC(127) - 16000) < 1);
  return 0;
}
