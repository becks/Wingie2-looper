// Input/output peak history for the Loop Monitor. The audio task adds blocks;
// the serial task reads the entries written since its previous read.
// Arduino-free so it can be tested on the host. GPL-3.0-or-later.
#pragma once
#include <atomic>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace wingie_loop {

class LevelMeter {
 public:
  // Input left/right, output left/right.
  static constexpr unsigned kChannels = 4;
  // One entry per 8 blocks of 64 samples: about 86 entries per second at 44.1 kHz.
  static constexpr unsigned kBlocksPerEntry = 8;
  // About 0.75 s of history; the monitor reads several times a second.
  static constexpr unsigned kSize = 64;
  // Entries near the write position may be overwritten while being copied.
  static constexpr unsigned kReadable = kSize - 8;

  // 0 is -60 dB or quieter, 240 is 0 dB (quarter-dB steps), 255 means clipped.
  static uint8_t encode(float peak) {
    if (!(peak > 0)) return 0;
    if (peak >= 0.999f) return 255;
    float steps = (20.0f * log10f(peak) + 60.0f) * 4.0f;
    if (steps <= 0) return 0;
    return steps >= 240 ? 240 : (uint8_t)steps;
  }

  // Audio task only. `signals` holds kChannels buffers of n samples.
  void addBlock(const float* const* signals, int n) {
    for (unsigned ch = 0; ch < kChannels; ++ch) {
      const float* x = signals[ch];
      float peak = peak_[ch];
      for (int i = 0; i < n; ++i) {
        float v = fabsf(x[i]);
        if (v > peak) peak = v;
      }
      peak_[ch] = peak;
    }
    if (++blocks_ < kBlocksPerEntry) return;
    blocks_ = 0;
    uint32_t head = head_.load(std::memory_order_relaxed);
    for (unsigned ch = 0; ch < kChannels; ++ch) {
      entries_[ch][head % kSize] = encode(peak_[ch]);
      peak_[ch] = 0;
    }
    head_.store(head + 1, std::memory_order_release);
  }

  // Copies up to `max` entries written after `from` into out[ch][0..count) and
  // advances `from`. Entries older than kReadable are skipped.
  size_t read(uint32_t& from, uint8_t out[][kSize], size_t max) const {
    uint32_t head = head_.load(std::memory_order_acquire);
    if (head - from > kReadable) from = head - kReadable;
    if (head - from > max) from = head - (uint32_t)max;
    size_t count = head - from;
    for (size_t i = 0; i < count; ++i)
      for (unsigned ch = 0; ch < kChannels; ++ch) out[ch][i] = entries_[ch][(from + i) % kSize];
    from = head;
    return count;
  }

 private:
  uint8_t entries_[kChannels][kSize] = {};
  float peak_[kChannels] = {};
  unsigned blocks_ = 0;
  std::atomic<uint32_t> head_{0};
};

}  // namespace wingie_loop
