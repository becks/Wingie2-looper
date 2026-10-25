#include "Wingie2/resonator_modes.h"
#include "Wingie2/config_profiles.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

using namespace wingie_modes;

static bool near(float a, float b) { return fabsf(a - b) < 1e-4f; }

int main() {
  // The protocol limits cover exactly the modes this header knows.
  assert(wingie_config::kModeMax + 1 == kModeCount);
  assert(wingie_config::kModeCycleMax == kAllModesMask);
  assert(kClassicModesMask == 0x1F && kAllModesMask == 0x7FFFF && kDefaultCycleMask == kAllModesMask);

  // Pitched modes: String, Bar and everything after Ratio; not Poly, Cave, Ratio.
  assert(!isPitchedMono(kPoly) && !isPitchedMono(kCave) && !isPitchedMono(kRatio));
  assert(isPitchedMono(kString) && isPitchedMono(kBar));
  for (int mode = kPiano; mode < kModeCount; mode++) assert(isPitchedMono(mode));
  assert(!isPitchedMono(kModeCount) && !isPitchedMono(-1));

  // String and Bar keep their original partials.
  for (uint8_t i = 0; i < kPartialCount; i++) {
    assert(near(partialRatio(kString, i, nullptr), i + 1.0f));
    assert(near(partialRatio(kBar, i, nullptr), 0.44444f * (i + 1.5f) * (i + 1.5f)));
  }
  // Every table rises (no two partials stacked) and stays inside the Ratio limits.
  for (int mode = kPiano; mode < kCustom; mode++) {
    for (uint8_t i = 0; i < kPartialCount; i++) {
      const float ratio = partialRatio(mode, i, nullptr);
      assert(ratio >= wingie_config::kRatioMin && ratio <= wingie_config::kRatioMax + 5);
      if (i) assert(ratio > partialRatio(mode, i - 1, nullptr));
    }
  }
  assert(near(partialRatio(kTube, 3, nullptr), 7) && near(partialRatio(kBell, 0, nullptr), 0.5f));
  assert(near(partialRatio(kMembrane, 1, nullptr), 1.593f));
  // Consonant tables: just-intonation intervals, and Chorus detuned by 6 cents.
  assert(near(partialRatio(kFifths, 8, nullptr), 12) && near(partialRatio(kMajor9, 6, nullptr), 3.75f));
  assert(near(partialRatio(kChorus, 2, nullptr), powf(2.0f, 6.0f / 1200.0f)));
  assert(near(partialRatio(kChorus, 3, nullptr) * powf(2.0f, 6.0f / 1200.0f), 1.5f));
  for (int mode = kFifths; mode <= kTanpura; mode++) assert(isPitchedMono(mode));
  // Custom plays the Ratio profile on one note.
  const float custom[kPartialCount] = {1, 1.5f, 2, 2.5f, 3, 3.5f, 4, 4.5f, 5};
  for (uint8_t i = 0; i < kPartialCount; i++) assert(near(partialRatio(kCustom, i, custom), custom[i]));

  // The original five alone cycle as before; by default every mode is in the cycle.
  assert(nextModeInCycle(kPoly, kClassicModesMask) == kString);
  assert(nextModeInCycle(kRatio, kClassicModesMask) == kPoly);
  // A mode outside the cycle (set over MIDI) steps to the next one in it.
  assert(nextModeInCycle(kBell, kClassicModesMask) == kPoly);
  const uint32_t mine = (1ul << kString) | (1ul << kBell) | (1ul << kCustom);
  assert(nextModeInCycle(kString, mine) == kBell);
  assert(nextModeInCycle(kBell, mine) == kCustom);
  assert(nextModeInCycle(kCustom, mine) == kString);
  assert(nextModeInCycle(kPoly, mine) == kString);
  // A single mode stays put; an empty or unknown mask falls back to the five.
  assert(nextModeInCycle(kGong, 1ul << kGong) == kGong);
  assert(sanitizeCycle(0) == kDefaultCycleMask && sanitizeCycle(0x80000000ul) == kDefaultCycleMask);
  assert(sanitizeCycle(0xFFFFFFFFul) == kAllModesMask);
  assert(nextModeInCycle(kPlate, kAllModesMask) == kFifths && nextModeInCycle(kTanpura, kAllModesMask) == kCustom);
  assert(nextModeInCycle(kRatio, kAllModesMask) == kPiano && nextModeInCycle(kCustom, kAllModesMask) == kPoly);

  printf("resonator_modes_test passed\n");
  return 0;
}
