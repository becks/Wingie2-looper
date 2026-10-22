// Independent mono loop engines. GPL-3.0-or-later.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
namespace wingie_loop {
// Display snapshot of one audio loop for the Loop Monitor.
struct LoopView {
  int state = 0;
  uint32_t length = 0, position = 0, capacity = 0;
  float speed = 1, level = 1, retention = 0.8f;
  bool reverse = false;
};
class Loop {
public:
  // Storage is a table of fixed-size chunks so one loop can span several
  // separate internal-RAM heap regions. 2 KB chunks also fill the small
  // fragments a 4 KB chunk cannot; 64 of them still hold ~26 s.
  static constexpr unsigned kChunkBits = 11;
  static constexpr size_t kChunkSize = size_t(1) << kChunkBits;
  static constexpr size_t kMaxChunks = 64;
private:
  static constexpr unsigned kRateDivision = 9;
  uint8_t* chunks_[kMaxChunks] = {};
  size_t capacity_ = 0, length_ = 0;
  // Integer frame plus fraction: a single float position cannot represent the
  // speed/9 step accurately once loops grow past a few tens of thousands of frames.
  size_t position_ = 0;
  float fraction_ = 0, smooth_ = 0;
  float recordSum_ = 0, overdubInputSum_ = 0, overdubFeedbackSum_ = 0;
  unsigned recordCount_ = 0, overdubCount_ = 0;
  size_t overdubIndex_ = 0;
  int state_ = 0;
  bool filled_ = false, wrapped_ = false;
  uint8_t& at(size_t i) { return chunks_[i >> kChunkBits][i & (kChunkSize - 1)]; }
  static uint8_t encode(float sample) {
    // This runs once for each 4.9 kHz looper frame while overdubbing.  Avoid
    // the floating-point rounding helper and the eight-step exponent scan so
    // the audio callback keeps enough margin for the I2S DMA deadline.
    int pcm = (int)(limit(sample) * 32767.0f);
    int sign = (pcm < 0) ? 0x80 : 0;
    if (pcm < 0) pcm = -pcm;
    if (pcm > 32635) pcm = 32635;
    unsigned magnitude = (unsigned)(pcm + 0x84);
    int exponent = (31 - __builtin_clz(magnitude)) - 7;
    if (exponent < 0) exponent = 0;
    if (exponent > 7) exponent = 7;
    int mantissa = (magnitude >> (exponent + 3)) & 0x0f;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
  }
  static float decode(uint8_t code) {
    int value = (uint8_t)~code;
    int sample = (((value & 0x0f) << 3) + 0x84) << ((value >> 4) & 0x07);
    sample -= 0x84;
    return (value & 0x80 ? -sample : sample) / 32768.0f;
  }
  void finishRecording() {
    if (recordCount_ && length_ < capacity_)
      at(length_++) = encode(recordSum_ / recordCount_);
    recordSum_ = 0;
    recordCount_ = 0;
  }
  void finishOverdub() {
    if (!overdubCount_ || overdubIndex_ >= length_) return;
    float input = overdubInputSum_ / overdubCount_;
    float feedback = overdubFeedbackSum_ / overdubCount_;
    at(overdubIndex_) = encode(decode(at(overdubIndex_)) * retention_ + input + feedback);
    overdubInputSum_ = 0;
    overdubFeedbackSum_ = 0;
    overdubCount_ = 0;
  }
  float retention_ = 0.8f;
  // Decoded neighbours of the current frame; they change only every ~9 samples.
  static constexpr size_t kNoCache = ~size_t(0);
  size_t cachedIndex_ = kNoCache;
  float cachedA_ = 0, cachedB_ = 0;
  // Block processing keeps the hot playback state in locals: the audio task has
  // only a few percent of headroom above the Faust resonators.
  template <bool kReverse>
  void processBlock(const float* input, float* output, int n, float step, float level,
                    float retention, const float* feedback, float cross) {
    if (!capacity_ || state_ == 0) {
      for (int k = 0; k < n; ++k) output[k] = input[k];
      return;
    }
    int k = 0;
    for (; k < n && state_ == 1; ++k) {
      output[k] = input[k];
      recordSum_ += limit(input[k]);
      if (++recordCount_ == kRateDivision) {
        at(length_++) = encode(recordSum_ * (1.0f / kRateDivision));
        recordSum_ = 0;
        recordCount_ = 0;
      }
      if (length_ == capacity_) {
        state_ = 2; position_ = 0; fraction_ = 0; filled_ = true;
        cachedIndex_ = kNoCache;
      }
    }
    if (k == n) return;
    const size_t length = length_;
    const bool overdub = state_ == 3;
    const float fadeLength = length < 32 ? length * 0.25f : 8.0f;
    const float fadeScale = 1.0f / fadeLength;
    size_t position = position_, cached = cachedIndex_;
    float fraction = fraction_, smooth = smooth_, a = cachedA_, b = cachedB_;
    if (overdub) retention_ = retention;
    for (; k < n; ++k) {
      if (position != cached) {
        size_t next = position + 1 == length ? 0 : position + 1;
        a = decode(at(position));
        b = decode(at(next));
        cached = position;
      }
      float sample = a + (b - a) * fraction;
      // Short edge fade suppresses discontinuities without changing loop duration.
      size_t back = length - position;
      if (position < 9 || back <= 9) {
        float edge = position < back ? position + fraction : back - fraction;
        if (edge < fadeLength) sample *= edge * fadeScale;
      }
      if (overdub) {
        if (!overdubCount_) overdubIndex_ = position;
        if (position != overdubIndex_) {
          finishOverdub();
          overdubIndex_ = position;
          cached = kNoCache;  // the rewritten frame may be one of the neighbours
        }
        // Codec input is already finite and in range; re-checking it on every
        // 44.1 kHz sample is needlessly expensive during overdub.
        overdubInputSum_ += input[k];
        if (feedback && cross != 0) overdubFeedbackSum_ += cross * feedback[k];
        overdubCount_++;
      }
      if (kReverse) {
        fraction -= step;
        while (fraction < 0) {
          fraction += 1;
          if (position) --position;
          else { position = length - 1; wrapped_ = true; }
        }
      } else {
        fraction += step;
        while (fraction >= 1) {
          fraction -= 1;
          if (++position >= length) { position = 0; wrapped_ = true; }
        }
      }
      smooth += 0.02f * (level - smooth);
      // During overdub, let the player hear the new material immediately as
      // well as the existing loop. -6 dB keeps their combined drive controlled.
      output[k] = overdub ? sample * smooth + input[k] * 0.5f : sample * smooth;
    }
    position_ = position;
    fraction_ = fraction;
    smooth_ = smooth;
    cachedIndex_ = cached;
    cachedA_ = a;
    cachedB_ = b;
  }
public:
  void attach(uint8_t* const* chunks, size_t count) {
    if (count > kMaxChunks) count = kMaxChunks;
    for (size_t c = 0; c < count; ++c) chunks_[c] = chunks[c];
    capacity_ = count * kChunkSize;
  }
  // Contiguous buffer convenience, used by the host tests.
  void attach(uint8_t* data, size_t capacity) {
    if (!data) capacity = 0;
    if (capacity > kMaxChunks * kChunkSize) capacity = kMaxChunks * kChunkSize;
    for (size_t c = 0; c * kChunkSize < capacity; ++c) chunks_[c] = data + c * kChunkSize;
    capacity_ = capacity;
  }
  size_t length() const { return length_; }
  size_t position() const { return position_; }
  size_t capacity() const { return capacity_; }
  // Peak magnitude (0-127, mu-law segment and step) of each of `buckets` equal
  // slices of the loop. Called from another task while audio runs, so it may
  // see a frame mid-update: display use only.
  void peaks(uint8_t* out, size_t buckets) const {
    size_t length = length_;
    for (size_t b = 0; b < buckets; ++b) {
      size_t begin = length * b / buckets, end = length * (b + 1) / buckets;
      uint8_t peak = 0;
      for (size_t i = begin; i < end; ++i) {
        uint8_t magnitude = (uint8_t)~chunks_[i >> kChunkBits][i & (kChunkSize - 1)] & 0x7f;
        if (magnitude > peak) peak = magnitude;
      }
      out[b] = peak;
    }
  }
  int state() const { return state_; }
  // True once after recording filled the buffer and switched itself to playback.
  bool takeFilled() { bool filled = filled_; filled_ = false; return filled; }
  // True once after playback or overdub wrapped around to the loop start.
  bool takeWrapped() { bool wrapped = wrapped_; wrapped_ = false; return wrapped; }
  bool active() const { return capacity_ && state_ != 0; }
  static constexpr unsigned rateDivision() { return kRateDivision; }
  // 0 bypass, 1 replace-record, 2 play, 3 overdub, 4 clear.
  void command(int value) {
    if (!capacity_) return;
    cachedIndex_ = kNoCache;
    if (value == 4) {
      length_ = 0; position_ = 0; fraction_ = 0; state_ = 0;
      recordSum_ = overdubInputSum_ = overdubFeedbackSum_ = 0;
      recordCount_ = overdubCount_ = 0;
    }
    else if (value == 1) {
      if (state_ != 1) {
        length_ = 0; position_ = 0; fraction_ = 0; recordSum_ = 0; recordCount_ = 0;
        overdubInputSum_ = overdubFeedbackSum_ = 0; overdubCount_ = 0;
      }
      state_ = 1;
    }
    else if (value >= 0 && value <= 3) {
      if (state_ == 1) finishRecording();
      if (state_ == 3) finishOverdub();
      state_ = (value >= 2 && !length_) ? 0 : value;
      if (state_ >= 2 && position_ >= length_) { position_ = 0; fraction_ = 0; }
      if (state_ == 3) overdubCount_ = 0;
    }
  }
  static float limit(float x) {
    if (!isfinite(x)) return 0;
    if (x < -1) return -1;
    if (x > 1) return 1;
    return x;
  }
  // Processes n samples; `step` is speed / rateDivision(), computed once per
  // block. `feedback` (scaled by `cross`) may be null when cross-feedback is off.
  void process(const float* input, float* output, int n, float step, float level,
               float retention, const float* feedback, float cross) {
    processBlock<false>(input, output, n, step, level, retention, feedback, cross);
  }
  void processReverse(const float* input, float* output, int n, float step, float level,
                      float retention, const float* feedback, float cross) {
    processBlock<true>(input, output, n, step, level, retention, feedback, cross);
  }
  // Single-sample convenience used by the host tests.
  float process(float input, float speed, float level, float retention, float feedback) {
    float output;
    process(&input, &output, 1, speed / kRateDivision, level, retention, &feedback, 1);
    return output;
  }
  float processReverse(float input, float speed, float level, float retention, float feedback) {
    float output;
    processReverse(&input, &output, 1, speed / kRateDivision, level, retention, &feedback, 1);
    return output;
  }
};
}
