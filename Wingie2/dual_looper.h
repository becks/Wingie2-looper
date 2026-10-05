// Independent mono loop engines. GPL-3.0-or-later.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
namespace wingie_loop {
class Loop {
  static constexpr unsigned kRateDivision = 9;
  uint8_t* data_ = nullptr;
  size_t capacity_ = 0, length_ = 0;
  float phase_ = 0, smooth_ = 0;
  float recordSum_ = 0, overdubInputSum_ = 0, overdubFeedbackSum_ = 0;
  unsigned recordCount_ = 0, overdubCount_ = 0;
  size_t overdubIndex_ = 0;
  int state_ = 0;
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
      data_[length_++] = encode(recordSum_ / recordCount_);
    recordSum_ = 0;
    recordCount_ = 0;
  }
  void finishOverdub() {
    if (!overdubCount_ || overdubIndex_ >= length_) return;
    float input = overdubInputSum_ / overdubCount_;
    float feedback = overdubFeedbackSum_ / overdubCount_;
    data_[overdubIndex_] = encode(decode(data_[overdubIndex_]) * retention_ + input + feedback);
    overdubInputSum_ = 0;
    overdubFeedbackSum_ = 0;
    overdubCount_ = 0;
  }
  float retention_ = 0.9f;
  template <bool kReverse>
  float processDirection(float input, float speed, float level, float retention, float feedback) {
    if (!data_ || state_ == 0) return input;
    if (state_ == 1) {
      recordSum_ += limit(input);
      if (++recordCount_ == kRateDivision) {
        data_[length_++] = encode(recordSum_ / kRateDivision);
        recordSum_ = 0;
        recordCount_ = 0;
      }
      if (length_ == capacity_) {
        state_ = 2; phase_ = 0;
      }
      return input;
    }
    size_t i = (size_t)phase_, j = i + 1;
    if (j == length_) j = 0;
    float fraction = phase_ - i;
    float sample = decode(data_[i]) * (1-fraction) + decode(data_[j]) * fraction;
    // Short edge fade suppresses discontinuities without changing loop duration.
    float edge = phase_ < (float)length_ - phase_ ? phase_ : (float)length_ - phase_;
    float fadeLength = length_ < 32 ? length_ * 0.25f : 8.0f;
    if (edge < fadeLength) sample *= edge / fadeLength;
    if (state_ == 3) {
      retention_ = retention;
      if (!overdubCount_) overdubIndex_ = i;
      if (i != overdubIndex_) {
        finishOverdub();
        overdubIndex_ = i;
      }
      // Codec input is already finite and in range; re-checking it on every
      // 44.1 kHz sample is needlessly expensive during overdub.
      overdubInputSum_ += input;
      if (feedback != 0) overdubFeedbackSum_ += feedback;
      overdubCount_++;
    }
    if (kReverse) {
      phase_ -= speed / kRateDivision;
      while (phase_ < 0) phase_ += length_;
    } else {
      phase_ += speed / kRateDivision;
      while (phase_ >= length_) phase_ -= length_;
    }
    smooth_ += 0.02f * (level - smooth_);
    // During overdub, let the player hear the new material immediately as
    // well as the existing loop. -6 dB keeps their combined drive controlled.
    if (state_ == 3) return sample * smooth_ + input * 0.5f;
    return sample * smooth_;
  }
public:
  void attach(uint8_t* data, size_t capacity) { data_ = data; capacity_ = capacity; }
  size_t length() const { return length_; }
  int state() const { return state_; }
  bool active() const { return data_ && state_ != 0; }
  static constexpr unsigned rateDivision() { return kRateDivision; }
  // 0 bypass, 1 replace-record, 2 play, 3 overdub, 4 clear.
  void command(int value) {
    if (!data_) return;
    if (value == 4) {
      length_ = 0; phase_ = 0; state_ = 0;
      recordSum_ = overdubInputSum_ = overdubFeedbackSum_ = 0;
      recordCount_ = overdubCount_ = 0;
    }
    else if (value == 1) {
      if (state_ != 1) {
        length_ = 0; phase_ = 0; recordSum_ = 0; recordCount_ = 0;
        overdubInputSum_ = overdubFeedbackSum_ = 0; overdubCount_ = 0;
      }
      state_ = 1;
    }
    else if (value >= 0 && value <= 3) {
      if (state_ == 1) finishRecording();
      if (state_ == 3) finishOverdub();
      state_ = (value >= 2 && !length_) ? 0 : value;
      if (state_ >= 2 && phase_ >= length_) phase_ = 0;
      if (state_ == 3) overdubCount_ = 0;
    }
  }
  static float limit(float x) {
    if (!isfinite(x)) return 0;
    if (x < -1) return -1;
    if (x > 1) return 1;
    return x;
  }
  // Keep normal playback on the original forward-only inner path. Reverse is
  // selected outside the per-sample loop by LooperDSP.
  float process(float input, float speed, float level, float retention, float feedback) {
    return processDirection<false>(input, speed, level, retention, feedback);
  }
  float processReverse(float input, float speed, float level, float retention, float feedback) {
    return processDirection<true>(input, speed, level, retention, feedback);
  }
};
}
