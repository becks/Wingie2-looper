// Mono bass on the final output: below the set frequency left and right are summed
// to the centre, above it the stereo image is untouched. GPL-3.0-or-later.
// Arduino-free so the host tests can include it.
//
// Mid/side: the side (L - R) / 2 goes through a 12 dB/octave Butterworth high pass,
// the mid (L + R) / 2 is left alone, so a centred signal comes out bit for bit and
// only the low part of the left/right difference is removed. One filter, about ten
// float operations per sample. Turning it on or off crossfades over one block.
#pragma once
#include <math.h>
#include "master_dynamics.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O2")
#endif
namespace wingie_master {

class MonoBass {
  float ic1_ = 0, ic2_ = 0;            // state-variable filter (Zavalishin, TPT)
  float a1_ = 0, a2_ = 0, a3_ = 0;
  static constexpr float kDamping = 1.41421356f;  // 1/Q, Q = 0.707 (Butterworth)
  int hzSet_ = -1;
  float rateSet_ = 0;
  bool on_ = false;
  void coefficients(int hz, float rate) {
    const float g = tanf(3.14159265f * hz / rate);
    a1_ = 1.0f / (1.0f + g * (g + kDamping));
    a2_ = g * a1_;
    a3_ = g * a2_;
    hzSet_ = hz;
    rateSet_ = rate;
  }
public:
  // hz: 0 off, else kMonoBassMin..kMonoBassMax (sanitize()).
  void process(float* left, float* right, int n, int hz, float rate) {
    const bool on = hz > 0;
    if (!on && !on_) return;
    if (on && (hz != hzSet_ || rate != rateSet_)) coefficients(hz, rate);
    if (on && !on_) ic1_ = ic2_ = 0;
    // Weight of the filtered side: 1 on, 0 off, a ramp across the switching block.
    float w = on_ ? 1.0f : 0.0f;
    const float dw = (on == on_ || n <= 0) ? 0.0f : (on ? 1.0f : -1.0f) / n;
    const float a1 = a1_, a2 = a2_, a3 = a3_;
    float ic1 = ic1_, ic2 = ic2_;
    for (int i = 0; i < n; ++i) {
      const float mid = 0.5f * (left[i] + right[i]);
      const float side = 0.5f * (left[i] - right[i]);
      const float v3 = side - ic2;
      const float v1 = a1 * ic1 + a2 * v3;
      const float v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2.0f * v1 - ic1;
      ic2 = 2.0f * v2 - ic2;
      const float high = side - kDamping * v1 - v2;
      w += dw;
      const float s = side + w * (high - side);
      left[i] = mid + s;
      right[i] = mid - s;
    }
    ic1_ = ic1;
    ic2_ = ic2;
    on_ = on;
  }
};

}  // namespace wingie_master
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
