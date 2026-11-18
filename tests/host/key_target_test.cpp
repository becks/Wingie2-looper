#include "Wingie2/key_target.h"
#include "Wingie2/config_profiles.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

using namespace wingie_keys;

static bool near(float a, float b) { return fabsf(a - b) < 1e-4f; }

int main() {
  // The protocol range covers exactly the targets.
  assert(wingie_config::kKeyTargetMin == kResonator && wingie_config::kKeyTargetMax + 1 == kKeyTargetCount);

  // Resonator only by default; loop only; both.
  assert(playsResonator(kResonator) && !playsLoop(kResonator));
  assert(!playsResonator(kLoop) && playsLoop(kLoop));
  assert(playsResonator(kBoth) && playsLoop(kBoth));

  // Panel C in the middle octave plays the loop as recorded; equal-tempered steps.
  assert(panelSemitones(0, 0) == 0 && panelSemitones(7, 0) == 7);
  assert(panelSemitones(0, -1) == -12 && panelSemitones(11, 1) == 23);
  assert(near(loopSpeedForSemitones(0), 1));
  assert(near(loopSpeedForSemitones(12), 2) && near(loopSpeedForSemitones(-12), 0.5f));
  assert(near(loopSpeedForSemitones(7), powf(2, 7 / 12.0f)));
  // The whole panel range (C at -1 to B at +1) fits the engine's 0.25-4x.
  for (int octave = -1; octave <= 1; octave++)
    for (int key = 0; key < 12; key++) {
      const float speed = loopSpeedForSemitones(panelSemitones(key, octave));
      assert(near(speed, powf(2, panelSemitones(key, octave) / 12.0f)));
    }
  // MIDI beyond two octaves clamps to the engine range.
  assert(near(loopSpeedForSemitones(36), wingie_loop::kEngineSpeedMax));
  assert(near(loopSpeedForSemitones(-36), wingie_loop::kSpeedMin));

  // MIDI: the side's panel C (48 left, 60 right) is the reference.
  assert(midiSemitones(48, 48) == 0 && midiSemitones(67, 60) == 7 && midiSemitones(36, 48) == -12);

  // CC 95 in three bands.
  assert(keyTargetFromCC(0) == kResonator && keyTargetFromCC(42) == kResonator);
  assert(keyTargetFromCC(43) == kLoop && keyTargetFromCC(85) == kLoop);
  assert(keyTargetFromCC(86) == kBoth && keyTargetFromCC(127) == kBoth && keyTargetFromCC(200) == kBoth);

  // The panel steps keep a key transposition above 2x instead of pulling it down.
  assert(near(wingie_loop::speedStepUp(3.0f), 3.0f) && near(wingie_loop::speedStepDown(3.0f), 2.0f));
  assert(near(wingie_loop::speedOctave(3.0f, true), 3.0f) && near(wingie_loop::speedOctave(3.0f, false), 1.5f));
  assert(near(wingie_loop::speedOctave(1.5f, true), 2.0f));

  printf("key_target_test passed\n");
  return 0;
}
