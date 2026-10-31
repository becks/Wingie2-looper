// Per-side output filter (low, high or band pass). GPL-3.0-or-later.
#pragma once
#include <math.h>
#include <stdint.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O2")
#endif
namespace wingie_loop {
enum FilterType : int { kFilterOff = 0, kFilterLow = 1, kFilterHigh = 2, kFilterBand = 3, kFilterTypeCount = 4 };
static constexpr float kFilterCutoffMin = 20.0f, kFilterCutoffMax = 16000.0f;

// MIDI CC values (0-127) to filter settings: type in four bands of 32, cutoff on
// an exponential scale over kFilterCutoffMin..Max, resonance 0-1.
inline int filterTypeFromCC(int value) { return value < 0 ? 0 : value > 127 ? 3 : value / 32; }
inline float filterCutoffFromCC(int value) {
  const float share = (value < 0 ? 0 : value > 127 ? 127 : value) / 127.0f;
  return kFilterCutoffMin * powf(kFilterCutoffMax / kFilterCutoffMin, share);
}

// Topology-preserving state-variable filter (Zavalishin): one structure gives the
// three responses, stays stable while the cutoff moves, and costs about ten
// float operations per sample. Coefficients are computed once per block; the
// cutoff glides between blocks so knob and CC steps do not zipper. Changing the
// type, or turning it on or off, crossfades over one block.
class SideFilter {
  float ic1_ = 0, ic2_ = 0;
  float logCutoff_ = -1;  // smoothed log2 of the cutoff; < 0 until first use
  int type_ = kFilterOff;
  // Coefficients for the current (smoothed) cutoff and resonance: recomputed only
  // while the cutoff glides or a setting changes, so a still filter costs no
  // transcendental calls (each tanf/exp2f/log2f is hundreds of cycles here).
  float cutoffSet_ = -1, resonanceSet_ = -1, logTarget_ = 0;
  int typeSet_ = kFilterOff;
  float a1_ = 0, a2_ = 0, a3_ = 0, k_ = 2;
  bool settled_ = false;
  // Resonance 0-1 maps to the damping k = 1/Q. Low and high pass: Q 0.5 .. ~6.7,
  // resonant but not self-oscillating. Band pass (unity gain at the centre) is
  // narrower, Q 1 .. 10: at Q 0.5 it would barely change the sound.
  static float damping(float resonance, int type) {
    resonance = resonance < 0 ? 0 : resonance > 1 ? 1 : resonance;
    if (type == kFilterBand) return 1.0f - 0.9f * resonance;
    return 2.0f - 1.85f * resonance;
  }
  static float pick(int type, float x, float low, float band, float high, float k) {
    if (type == kFilterLow) return low;
    if (type == kFilterHigh) return high;
    if (type == kFilterBand) return k * band;  // unity gain at the centre
    return x;
  }
  // The hot loop, one instantiation per response: nothing but the filter inside.
  template <int kType>
  void run(float* io, int n) {
    const float a1 = a1_, a2 = a2_, a3 = a3_, k = k_;
    float ic1 = ic1_, ic2 = ic2_;
    for (int i = 0; i < n; ++i) {
      const float x = io[i];
      const float v3 = x - ic2;
      const float v1 = a1 * ic1 + a2 * v3;
      const float v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2.0f * v1 - ic1;
      ic2 = 2.0f * v2 - ic2;
      io[i] = kType == kFilterLow ? v2 : kType == kFilterHigh ? x - k * v1 - v2 : k * v1;
    }
    ic1_ = ic1;
    ic2_ = ic2;
  }
  // Type change (or on/off): crossfade the old response into the new over the block.
  // `kFrom` is the old response's damping (band pass has its own range).
  void runFade(float* io, int n, int from, int to, float kFrom) {
    const float a1 = a1_, a2 = a2_, a3 = a3_, k = k_;
    float ic1 = ic1_, ic2 = ic2_;
    const float step = 1.0f / n;
    for (int i = 0; i < n; ++i) {
      const float x = io[i];
      const float v3 = x - ic2;
      const float v1 = a1 * ic1 + a2 * v3;
      const float v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2.0f * v1 - ic1;
      ic2 = 2.0f * v2 - ic2;
      const float t = (i + 1) * step;
      io[i] = t * pick(to, x, v2, v1, x - k * v1 - v2, k) +
              (1.0f - t) * pick(from, x, v2, v1, x - kFrom * v1 - v2, kFrom);
    }
    ic1_ = ic1;
    ic2_ = ic2;
  }
  void updateCoefficients(float cutoff, float resonance, int type, float sampleRate) {
    if (!(cutoff >= kFilterCutoffMin)) cutoff = kFilterCutoffMin;
    if (cutoff > kFilterCutoffMax) cutoff = kFilterCutoffMax;
    if (cutoff > 0.45f * sampleRate) cutoff = 0.45f * sampleRate;
    if (cutoff != cutoffSet_) {
      cutoffSet_ = cutoff;
      logTarget_ = log2f(cutoff);
      settled_ = false;
    }
    if (resonance != resonanceSet_ || type != typeSet_) {
      resonanceSet_ = resonance;
      typeSet_ = type;
      settled_ = false;
    }
    if (settled_) return;
    // About 30 ms to settle at 64-sample blocks; a fresh filter starts on target.
    if (logCutoff_ < 0) logCutoff_ = logTarget_;
    else logCutoff_ += 0.25f * (logTarget_ - logCutoff_);
    if (fabsf(logTarget_ - logCutoff_) < 0.001f) {
      logCutoff_ = logTarget_;
      settled_ = true;
    }
    const float g = tanf((float)M_PI * exp2f(logCutoff_) / sampleRate);
    k_ = damping(resonance, type);
    a1_ = 1.0f / (1.0f + g * (g + k_));
    a2_ = g * a1_;
    a3_ = g * a2_;
  }
public:
  void reset() { ic1_ = ic2_ = 0; }
  int type() const { return type_; }
  // Filters n samples in place. `sampleRate` in Hz, `cutoff` in Hz, `resonance` 0-1.
  void process(float* io, int n, int type, float cutoff, float resonance, float sampleRate) {
    if (type < 0 || type >= kFilterTypeCount) type = kFilterOff;
    const int previous = type_;
    type_ = type;
    if (type == kFilterOff && previous == kFilterOff) {
      ic1_ = ic2_ = 0;
      logCutoff_ = -1;
      settled_ = false;
      return;
    }
    if (n <= 0) return;
    const float kFrom = k_;
    updateCoefficients(cutoff, resonance, type, sampleRate);
    if (previous != type) runFade(io, n, previous, type, kFrom);
    else if (type == kFilterLow) run<kFilterLow>(io, n);
    else if (type == kFilterHigh) run<kFilterHigh>(io, n);
    else run<kFilterBand>(io, n);
    // A runaway state (non-finite input) restarts silent rather than staying NaN.
    if (!isfinite(ic1_) || !isfinite(ic2_)) ic1_ = ic2_ = 0;
  }
};
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
