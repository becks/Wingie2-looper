#include "Wingie2/speed_steps.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace wingie_loop;

static bool near(float a, float b) { return fabsf(a - b) < 1e-5f; }

int main() {
  // Steps are ordered and inside the looper range.
  for (int i = 1; i < kSpeedStepCount; ++i) assert(kSpeedSteps[i] > kSpeedSteps[i - 1]);
  assert(near(kSpeedSteps[0], kSpeedMin) && near(kSpeedSteps[kSpeedStepCount - 1], kSpeedMax));
  assert(near(kSpeedSteps[kUnisonStep], 1));
  assert(strcmp(speedStepName(kUnisonStep), "1x") == 0 && strcmp(speedStepName(8), "?") == 0);

  // Walking down from unison visits -4th, -5th, -oct, -2oct, then stays.
  float s = 1;
  const float down[] = {0.75f, 2.0f / 3.0f, 0.5f, 0.25f, 0.25f};
  for (float want : down) { s = speedStepDown(s); assert(near(s, want)); }
  s = 1;
  const float up[] = {4.0f / 3.0f, 1.5f, 2, 2};
  for (float want : up) { s = speedStepUp(s); assert(near(s, want)); }

  // Between steps (continuous CC 81) the next step in that direction is taken.
  assert(near(speedStepUp(1.1f), 4.0f / 3.0f) && near(speedStepDown(1.1f), 1));
  assert(near(speedStepUp(0.7f), 0.75f) && near(speedStepDown(0.7f), 2.0f / 3.0f));
  // A speed printed with 3 decimals still matches its step.
  assert(speedStepIndex(0.667f) == 2 && speedStepIndex(1.333f) == 5);
  assert(speedStepIndex(1.1f) == -1);
  assert(near(speedStepUp(1.333f), 1.5f) && near(speedStepDown(0.667f), 0.5f));

  // Octave jumps clamp to the range.
  assert(near(speedOctave(1, true), 2) && near(speedOctave(1, false), 0.5f));
  assert(near(speedOctave(1.5f, true), 2) && near(speedOctave(0.4f, false), 0.25f));
  assert(near(speedOctave(2.0f / 3.0f, false), 1.0f / 3.0f));

  // CC 86 bands: 64 is unison, the ends are the octave extremes.
  assert(speedStepFromCC(0) == 0 && speedStepFromCC(15) == 0 && speedStepFromCC(16) == 1);
  assert(speedStepFromCC(64) == kUnisonStep && speedStepFromCC(127) == 7);
  assert(speedStepFromCC(-3) == 0 && speedStepFromCC(200) == 7);

  printf("speed_steps_test passed\n");
  return 0;
}
