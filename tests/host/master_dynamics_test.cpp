// Register values of the AC101 master low cut and compressor (Wingie2/master_dynamics.h)
// against the worked examples of the AC101 user manual v1.1 (11.14.2).
#include "Wingie2/master_dynamics.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>

using namespace wingie_master;

static bool near(uint32_t a, uint32_t b, uint32_t tolerance) {
  return (a > b ? a - b : b - a) <= tolerance;
}

int main() {
  // Manual: T = -30 dB -> 0x04FB_9ED0 (also the register defaults ab/ac).
  assert(near(thresholdValue(-30), 0x04FB9ED0, 0x100));
  // Manual: n = 5 -> k = -0.8 -> 0x733_3333 (3.24, 27 bits).
  assert(near(slopeValue(5), 0x7333333, 1));
  // Register default K (ad/ae) 0x780_0000: 2:1.
  assert(slopeValue(2) == 0x7800000);
  // Manual: O = 6 dB -> 0x1FE_C982; register default O (af/b0) 0x100_0000 = 0 dB.
  assert(near(offsetValue(6), 0x1FEC982, 0x400));
  assert(offsetValue(0) == 0x1000000);
  // Codec default high pass coefficient, and a real low cut below 1.
  assert(lowCutCoefficient(0) == 0x00FFFAC1);
  const double a100 = lowCutCoefficient(100) / 16777216.0;
  assert(a100 > 0.985 && a100 < 0.987);  // exp(-2 pi 100 / 44100) = 0.98585
  // Time constants: 10 ms at 44.1 kHz is about 0.00498.
  const double e = timeCoefficient(10) / 16777216.0;
  assert(e > 0.0049 && e < 0.0051);

  // Writes cover a1..b0 in order, high register first.
  Settings s;
  s.lowCut = 80; s.threshold = -20; s.ratio = 4; s.makeup = 3;
  RegisterWrite w[kCoefficientWrites];
  coefficientWrites(s, w);
  for (int i = 0; i < kCoefficientWrites; ++i) assert(w[i].reg == 0xa1 + i);
  assert(((uint32_t)w[10].value << 16 | w[11].value) == thresholdValue(-20));  // ab/ac
  assert(((uint32_t)w[12].value << 16 | w[13].value) == slopeValue(4));        // ad/ae
  assert((w[0].value & ~0x7ff) == 0 && (w[12].value & ~0x7ff) == 0);           // 11-bit highs
  assert((w[14].value & ~0x1fff) == 0);                                        // 13-bit high

  // Enables: nothing on by default; each block its own bits.
  Settings off;
  assert(controlValue(off) == 0 && enableValue(off) == 0);
  Settings cut; cut.lowCut = 60;
  assert(controlValue(cut) == 0x3 && enableValue(cut) == 0xA080);
  Settings comp; comp.ratio = 3;
  assert(controlValue(comp) == 0x4);

  // Sanitize: out of range values clamp; a low cut under 20 Hz other than 0 is 20.
  Settings wild; wild.lowCut = 5; wild.threshold = -90; wild.ratio = 99; wild.makeup = -3;
  wild = sanitize(wild);
  assert(wild.lowCut == kLowCutMin && wild.threshold == kThresholdMin && wild.ratio == kRatioMax && wild.makeup == 0);
  Settings none; none.lowCut = -4;
  assert(sanitize(none).lowCut == 0);
  printf("master_dynamics_test passed\n");
}
