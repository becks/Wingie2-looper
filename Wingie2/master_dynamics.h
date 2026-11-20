// Master low cut and compressor in the AC101 codec's DAC processing (DAP): a
// first-order high pass and a dynamic range controller on both output channels,
// computed by the codec, so they cost the ESP32 no audio time. GPL-3.0-or-later.
// Arduino-free so the host tests can include it.
//
// Register formats (AC101 user manual v1.1, 11.14 and regs a0h-b5h): coefficients
// are two's complement fixed point split over a high register (upper bits) and a
// low register (16 bits): 3.24 for the high pass, energy, attack, decay and slope;
// 8.24 for the threshold; 5.24 for the offset.
#pragma once
#include <math.h>
#include <stdint.h>

namespace wingie_master {

// The codec's DAP runs at the I2S frame rate.
static constexpr float kSampleRate = 44100.0f;

// Low cut: 0 off, else the -3 dB frequency in Hz (6 dB/octave).
static constexpr int kLowCutMin = 20, kLowCutMax = 1000;
// Compressor: threshold in dBFS, ratio n:1 (1 = off), offset (makeup) in dB.
static constexpr int kThresholdMin = -40, kThresholdMax = 0, kDefaultThreshold = -20;
static constexpr int kRatioMin = 1, kRatioMax = 20, kDefaultRatio = 1;
static constexpr int kMakeupMin = 0, kMakeupMax = 12, kDefaultMakeup = 0;
// Gain attack (gain going down) and release in ms; the RMS window is fixed.
static constexpr int kAttackMin = 1, kAttackMax = 200, kDefaultAttack = 5;
static constexpr int kReleaseMin = 10, kReleaseMax = 2000, kDefaultRelease = 200;
static constexpr float kEnergyMs = 10.0f;
// Mono bass (computed by the ESP32, mono_bass.h, not the codec): 0 off, else the
// frequency in Hz below which left and right are summed to the centre.
static constexpr int kMonoBassMin = 40, kMonoBassMax = 300;

struct Settings {
  int lowCut = 0;
  int threshold = kDefaultThreshold;
  int ratio = kDefaultRatio;
  int makeup = kDefaultMakeup;
  int attack = kDefaultAttack;
  int release = kDefaultRelease;
  int monoBass = 0;
};

inline int clampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline Settings sanitize(Settings s) {
  s.lowCut = s.lowCut <= 0 ? 0 : clampInt(s.lowCut, kLowCutMin, kLowCutMax);
  s.threshold = clampInt(s.threshold, kThresholdMin, kThresholdMax);
  s.ratio = clampInt(s.ratio, kRatioMin, kRatioMax);
  s.makeup = clampInt(s.makeup, kMakeupMin, kMakeupMax);
  s.attack = clampInt(s.attack, kAttackMin, kAttackMax);
  s.release = clampInt(s.release, kReleaseMin, kReleaseMax);
  s.monoBass = s.monoBass <= 0 ? 0 : clampInt(s.monoBass, kMonoBassMin, kMonoBassMax);
  return s;
}
inline bool lowCutOn(const Settings& s) { return s.lowCut > 0; }
inline bool compressorOn(const Settings& s) { return s.ratio > 1; }

// value * 2^24 as two's complement in `bits` bits.
inline uint32_t fixed24(double value, int bits) {
  const double scaled = value * 16777216.0;
  int64_t raw = (int64_t)llround(scaled);
  const int64_t mask = ((int64_t)1 << bits) - 1;
  return (uint32_t)(raw & mask);
}

// High pass H(z) = (1 - z^-1) / (1 - a z^-1): a = exp(-2 pi fc / fs).
inline uint32_t lowCutCoefficient(int hz) {
  if (hz <= 0) return 0x00FFFAC1;  // the codec default (DC blocker, < 1 Hz)
  return fixed24(exp(-2.0 * M_PI * hz / kSampleRate), 27);
}
// Alpha filters (energy, attack, decay): a = 1 - exp(-2.2 Ts / t).
inline uint32_t timeCoefficient(float ms) {
  return fixed24(1.0 - exp(-2.2 / (kSampleRate * ms / 1000.0)), 27);
}
// Threshold: T = -dB / 6.0206 (8.24).
inline uint32_t thresholdValue(int db) { return fixed24(-db / 6.0206, 32); }
// Slope within compression: k = 1/n - 1 (3.24).
inline uint32_t slopeValue(int ratio) { return fixed24(1.0 / ratio - 1.0, 27); }
// Offset of the static curve: 10^(dB/20) (5.24).
inline uint32_t offsetValue(int db) { return fixed24(pow(10.0, db / 20.0), 29); }

struct RegisterWrite { uint8_t reg; uint16_t value; };

// Coefficient writes for both blocks (0xa1-0xb0), high register first.
static constexpr int kCoefficientWrites = 16;
inline void coefficientWrites(const Settings& s, RegisterWrite out[kCoefficientWrites]) {
  const uint32_t values[8] = {
    lowCutCoefficient(s.lowCut),
    timeCoefficient(kEnergyMs), timeCoefficient(kEnergyMs),
    timeCoefficient((float)s.release), timeCoefficient((float)s.attack),
    thresholdValue(s.threshold), slopeValue(s.ratio), offsetValue(s.makeup),
  };
  for (int i = 0; i < 8; ++i) {
    out[2 * i] = {(uint8_t)(0xa1 + 2 * i), (uint16_t)(values[i] >> 16)};
    out[2 * i + 1] = {(uint8_t)(0xa2 + 2 * i), (uint16_t)(values[i] & 0xffff)};
  }
}
// 0xa0: bit 2 DRC, bit 1 left high pass, bit 0 right high pass.
inline uint16_t controlValue(const Settings& s) {
  return (compressorOn(s) ? 0x4 : 0) | (lowCutOn(s) ? 0x3 : 0);
}
// 0xb5: the DAC path's DAP (as the AC101 Linux driver enables it).
inline uint16_t enableValue(const Settings& s) {
  return controlValue(s) ? 0xA080 : 0;
}
// Module clock/reset bit for "HPF & DRC" in regs 04h/05h.
static constexpr uint16_t kDapModuleBit = 1 << 6;

}  // namespace wingie_master
