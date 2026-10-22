#include "Wingie2/level_meter.h"
#include <cassert>
using wingie_loop::LevelMeter;

static void addBlocks(LevelMeter& meter, float inL, float inR, float outL, float outR, unsigned blocks) {
  float buffers[4][64];
  for (int i = 0; i < 64; ++i) {
    // Peak only on one sample: the meter must catch it.
    buffers[0][i] = i == 10 ? inL : 0;
    buffers[1][i] = i == 20 ? -inR : 0;
    buffers[2][i] = outL;
    buffers[3][i] = outR;
  }
  const float* signals[4] = {buffers[0], buffers[1], buffers[2], buffers[3]};
  for (unsigned b = 0; b < blocks; ++b) meter.addBlock(signals, 64);
}

int main() {
  assert(LevelMeter::encode(0) == 0);
  assert(LevelMeter::encode(0.0005f) == 0);    // below -60 dB
  assert(LevelMeter::encode(0.5f) == 215);     // -6.02 dB
  assert(LevelMeter::encode(0.998f) == 239);
  assert(LevelMeter::encode(1.0f) == 255);
  assert(LevelMeter::encode(3.0f) == 255);

  LevelMeter meter;
  uint32_t from = 0;
  uint8_t out[4][LevelMeter::kSize];
  assert(meter.read(from, out, 40) == 0);

  // One entry per kBlocksPerEntry blocks, peak held across them.
  addBlocks(meter, 0.5f, 0.1f, 1.5f, 0, LevelMeter::kBlocksPerEntry - 1);
  assert(meter.read(from, out, 40) == 0);
  addBlocks(meter, 0, 0, 0, 0, 1);
  assert(meter.read(from, out, 40) == 1);
  assert(out[0][0] == 215 && out[1][0] == 160 && out[2][0] == 255 && out[3][0] == 0);
  assert(meter.read(from, out, 40) == 0);

  // A slow reader gets only the newest entries.
  addBlocks(meter, 0.5f, 0, 0, 0, LevelMeter::kBlocksPerEntry * 100);
  assert(meter.read(from, out, 40) == 40);
  addBlocks(meter, 0.5f, 0, 0, 0, LevelMeter::kBlocksPerEntry * 100);
  assert(meter.read(from, out, 1000) == LevelMeter::kReadable);
  assert(out[0][0] == 215 && out[0][LevelMeter::kReadable - 1] == 215);
}
