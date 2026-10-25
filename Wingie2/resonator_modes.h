#ifndef WINGIE2_RESONATOR_MODES_H
#define WINGIE2_RESONATOR_MODES_H

#include <stdint.h>

// Resonator modes: which frequencies the nine resonators of a side ring at.
// Poly, String, Bar, Cave and Ratio are the original five; the others are
// pitched like String and Bar (one note, nine partials) with other partial
// tables. Arduino-free so the host tests can include it.
namespace wingie_modes {

enum Mode : uint8_t {
  kPoly = 0,
  kString,
  kBar,
  kCave,
  kRatio,
  kPiano,     // stiff string: harmonics slightly stretched
  kTube,      // closed tube: odd harmonics only
  kMembrane,  // circular membrane (drum head)
  kBell,      // church bell partials, hum an octave below
  kGlass,     // wine glass / thin ring
  kGong,      // free circular plate
  kPlate,     // rectangular plate
  // Consonant tables for pads and drones (just intonation).
  kFifths,    // octaves and fifths over four octaves
  kMajor9,    // open major 7/9 voicing
  kMinor11,   // minor 7/9/11
  kAir,       // the root with high harmonics 7-16
  kChorus,    // root, fifth and octave, each at -6, 0 and +6 cents
  kTanpura,   // Pa-Sa drone with harmonics
  kCustom,    // the nine Ratio profile ratios on one note
  kModeCount
};

static const uint8_t kPartialCount = 9;
static const uint32_t kAllModesMask = (1ul << kModeCount) - 1;
static const uint32_t kClassicModesMask = (1ul << (kRatio + 1)) - 1;
// The Mode button steps through every mode unless the configuration trims it.
static const uint32_t kDefaultCycleMask = kAllModesMask;
static const float kBarFactor = 0.44444f;

inline bool isPitchedMono(int mode) {
  return mode == kString || mode == kBar || (mode >= kPiano && mode < kModeCount);
}

// Keeps only known modes; an empty cycle falls back to the default.
inline uint32_t sanitizeCycle(uint32_t mask) {
  const uint32_t known = mask & kAllModesMask;
  return known ? known : kDefaultCycleMask;
}

// The mode after `current` in the cycle, wrapping; `current` when it is the only one.
inline int nextModeInCycle(int current, uint32_t mask) {
  mask = sanitizeCycle(mask);
  for (int step = 1; step <= kModeCount; step++) {
    const int mode = (current + step) % kModeCount;
    if (mask & (1ul << mode)) return mode;
  }
  return current;
}

static const float kPianoRatios[kPartialCount] = {1.0f, 2.007f, 3.03f, 4.074f, 5.147f, 6.256f, 7.407f, 8.606f, 9.857f};
static const float kTubeRatios[kPartialCount] = {1.0f, 3.0f, 5.0f, 7.0f, 9.0f, 11.0f, 13.0f, 15.0f, 17.0f};
static const float kMembraneRatios[kPartialCount] = {1.0f, 1.593f, 2.136f, 2.296f, 2.653f, 2.918f, 3.156f, 3.501f, 3.6f};
static const float kBellRatios[kPartialCount] = {0.5f, 1.0f, 1.189f, 1.5f, 2.0f, 2.514f, 2.662f, 3.011f, 4.166f};
static const float kGlassRatios[kPartialCount] = {1.0f, 2.828f, 5.423f, 8.771f, 12.866f, 17.709f, 23.297f, 29.632f, 36.712f};
static const float kGongRatios[kPartialCount] = {1.0f, 1.73f, 2.33f, 3.91f, 4.11f, 6.3f, 6.92f, 7.43f, 8.88f};
static const float kPlateRatios[kPartialCount] = {1.0f, 1.829f, 3.171f, 3.211f, 4.0f, 5.146f, 5.382f, 6.789f, 7.317f};
static const float kFifthsRatios[kPartialCount] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f};
static const float kMajor9Ratios[kPartialCount] = {0.5f, 1.0f, 1.5f, 2.25f, 2.5f, 3.0f, 3.75f, 4.5f, 6.0f};
static const float kMinor11Ratios[kPartialCount] = {0.5f, 1.0f, 1.5f, 2.4f, 3.0f, 3.6f, 4.5f, 5.333f, 6.0f};
static const float kAirRatios[kPartialCount] = {1.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 16.0f};
// 2^(6/1200) = 1.003471: about 0.5 Hz of beating on the lowest C.
static const float kChorusRatios[kPartialCount] = {0.996542f, 1.0f, 1.003471f, 1.494813f, 1.5f, 1.505207f, 1.993084f, 2.0f, 2.006943f};
static const float kTanpuraRatios[kPartialCount] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};

// Partial `index` of a pitched mode as a ratio to the note's frequency.
// `customRatios` (the Ratio profile) serves Custom and any non-pitched mode.
inline float partialRatio(int mode, uint8_t index, const float *customRatios) {
  switch (mode) {
    case kString: return index + 1.0f;
    case kBar: {
      const float barIndex = index + 1.5f;
      return kBarFactor * barIndex * barIndex;
    }
    case kPiano: return kPianoRatios[index];
    case kTube: return kTubeRatios[index];
    case kMembrane: return kMembraneRatios[index];
    case kBell: return kBellRatios[index];
    case kGlass: return kGlassRatios[index];
    case kGong: return kGongRatios[index];
    case kPlate: return kPlateRatios[index];
    case kFifths: return kFifthsRatios[index];
    case kMajor9: return kMajor9Ratios[index];
    case kMinor11: return kMinor11Ratios[index];
    case kAir: return kAirRatios[index];
    case kChorus: return kChorusRatios[index];
    case kTanpura: return kTanpuraRatios[index];
    default: return customRatios[index];
  }
}

}  // namespace wingie_modes

#endif
