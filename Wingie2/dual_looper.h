// Independent mono loop engines. GPL-3.0-or-later.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
// The engine runs in the audio task next to the Faust resonators; the sketch's
// default -Os leaves its per-sample loop noticeably slower than -O2.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O2")
#endif
namespace wingie_loop {
// What holds a side's gate open (bits): panel keys, MIDI notes, the note looper.
enum GateSource : uint8_t { kGatePanel = 1, kGateMidi = 2, kGateNoteLoop = 4 };
// Display snapshot of one audio loop for the Loop Monitor.
struct LoopView {
  int state = 0;
  uint32_t length = 0, position = 0, capacity = 0;
  float speed = 1, level = 1, retention = 0.8f;
  bool reverse = false;
  int speedMode = 0;  // wingie_loop::SpeedMode
  bool post = false, gate = false;
  float start = 0, end = 1;  // playback window, shares of the length
  // Side output filter (kept here so one view carries all per-side looper state).
  int filterType = 0;
  float filterCutoff = 1000, filterResonance = 0;
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
  uint8_t get(size_t i) const { return chunks_[i >> kChunkBits][i & (kChunkSize - 1)]; }
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
    if (!overdubCount_ || overdubIndex_ >= span_ || begin_ + overdubIndex_ >= length_) return;
    float input = overdubInputSum_ / overdubCount_;
    float feedback = overdubFeedbackSum_ / overdubCount_;
    uint8_t& frame = at(begin_ + overdubIndex_);
    frame = encode(decode(frame) * retention_ + input + feedback);
    overdubInputSum_ = 0;
    overdubFeedbackSum_ = 0;
    overdubCount_ = 0;
  }
  float retention_ = 0.8f;
  // Playback window: shares of the recording (0-1) set by the control side, and the
  // frames they cover for the current length. Playback, grains, edge fades and
  // overdub all run on positions relative to begin_; the recording stays whole.
  static constexpr size_t kMinSpan = 64;  // ~13 ms
  float windowStart_ = 0, windowEnd_ = 1;
  size_t begin_ = 0, span_ = 0;
  // The window moved away from the play position: fade out (as a gate restart),
  // then enter the new window at its start on the next block.
  bool jump_ = false, jumpReady_ = false;
  void windowFrames(size_t& begin, size_t& span) const {
    const size_t length = length_;
    size_t b = (size_t)(windowStart_ * length), e = (size_t)(windowEnd_ * length + 0.5f);
    if (b > length) b = length;
    if (e > length) e = length;
    const size_t min = length < kMinSpan ? length : kMinSpan;
    if (e < b + min) {
      e = b + min;
      if (e > length) { e = length; b = length - min; }
    }
    begin = b;
    span = e - b;
  }
  // Takes the current window; a play position outside it starts it over (no fade:
  // used on transport changes) or, from playback, requests the faded jump.
  void updateWindow(bool fade, bool reverse) {
    size_t begin, span;
    windowFrames(begin, span);
    if (jumpReady_) {
      jump_ = jumpReady_ = false;
      enterWindow(begin, span, reverse);
      return;
    }
    if (begin == begin_ && span == span_) return;
    if (!span_) fade = false;  // no window yet (a recording just closed)
    const size_t absolute = begin_ + position_;
    if (!jump_ && absolute >= begin && absolute < begin + span) {
      finishOverdub();
      overdubCount_ = 0;
      position_ = absolute - begin;
      begin_ = begin;
      span_ = span;
      cachedIndex_ = kNoCache;
      headsPlaced_ = false;  // re-placed at the same offsets from the play position
    } else if (fade) {
      jump_ = true;  // keeps the old window until the fade is silent
    } else {
      jump_ = false;
      enterWindow(begin, span, reverse);
    }
  }
  void enterWindow(size_t begin, size_t span, bool reverse) {
    finishOverdub();
    overdubCount_ = 0;
    begin_ = begin;
    span_ = span;
    position_ = reverse && span ? span - 1 : 0;
    fraction_ = reverse ? 0.9999f : 0;
    cachedIndex_ = kNoCache;
    headsPlaced_ = false;
  }
  // Live input mixed into playback (0 = the loop replaces it); overdub always uses -6 dB.
  float liveInput_ = 0;
  // After the resonator the input is the resonator output, which always passes at unity.
  bool post_ = false;
  // Gate mode: playback is heard only while the gate is open, and a trigger
  // restarts the loop once the gate has faded out (~2 ms), so retriggers do not click.
  static constexpr float kGateRate = 0.05f, kGateFloor = 0.01f;
  bool gateMode_ = false, gateOpen_ = false, restart_ = false;
  float gate_ = 1;
  // Decoded neighbours of the current frame; they change only every ~9 samples.
  static constexpr size_t kNoCache = ~size_t(0);
  size_t cachedIndex_ = kNoCache;
  float cachedA_ = 0, cachedB_ = 0;
  // Pitch shift without changing loop length: two read heads ("grains") at an
  // offset ahead of the play position that drifts at (pitch - 1); they sit half a
  // grain apart with triangular gains summing to 1, so each wraps while silent.
  static constexpr float kGrainFrames = 320;  // ~65 ms at 4.9 kHz
  // Each grain is a read head moving at the pitch rate; like the play position
  // it decodes its two neighbouring frames only when it moves to a new frame.
  // A head is re-placed relative to the play position when its offset wraps.
  // Positions are fixed point (kFxBits fraction bits): the audio task cannot
  // afford float position loops for two extra heads per side.
  static constexpr int kFxBits = 12;
  static constexpr float kFxOne = 1 << kFxBits, kFxInv = 1.0f / (1 << kFxBits);
  struct Head {
    int32_t position = 0;  // frames << kFxBits
    size_t cached = kNoCache;
    float a = 0, b = 0;
  };
  Head heads_[2];
  int32_t grain_ = 0;  // grain phase, frames << kFxBits
  bool headsPlaced_ = false;
  // Grains run at half the audio rate (the loop holds nothing above ~2.4 kHz):
  // computed on alternate samples and linearly interpolated in between.
  bool grainSkip_ = false;
  float grainLast_ = 0;
  __attribute__((always_inline)) static void place(Head& head, size_t position, float fraction,
                                                   int32_t offset, int32_t lengthFx) {
    int32_t p = ((int32_t)position << kFxBits) + (int32_t)(fraction * kFxOne) + offset;
    if (p >= lengthFx) p -= lengthFx;
    head.position = p;
  }
  __attribute__((always_inline)) float read(Head& head, size_t begin, size_t length, float fadeScale, float fadeLength) const {
    size_t i = (size_t)(head.position >> kFxBits);
    if (i != head.cached) {
      head.a = decode(get(begin + i));
      head.b = decode(get(begin + (i + 1 == length ? 0 : i + 1)));
      head.cached = i;
    }
    float fraction = (head.position & ((1 << kFxBits) - 1)) * kFxInv;
    float sample = head.a + (head.b - head.a) * fraction;
    if (i < 9 || i + 9 >= length) {
      size_t back = length - i;
      float edge = i < back ? i + fraction : back - fraction;
      if (edge < fadeLength) sample *= edge * fadeScale;
    }
    return sample;
  }
  __attribute__((always_inline)) static void advance(Head& head, int32_t delta, int32_t lengthFx) {
    int32_t p = head.position + delta;
    if (p >= lengthFx) p -= lengthFx;
    else if (p < 0) p += lengthFx;
    head.position = p;
  }
  // Block processing keeps the hot playback state in locals: the audio task has
  // only a few percent of headroom above the Faust resonators.
  template <bool kReverse, bool kGrains>
  void processBlock(const float* input, float* output, int n, float step, float level,
                    float retention, const float* feedback, float cross, float pitch) {
    bool restart = restart_;
    restart_ = false;
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
    updateWindow(true, kReverse);
    // From here on `length` is the window and positions are relative to `begin`.
    const size_t begin = begin_, length = span_;
    const bool overdub = state_ == 3;
    const float fadeLength = length < 32 ? length * 0.25f : 8.0f;
    const float fadeScale = 1.0f / fadeLength;
    size_t position = position_, cached = cachedIndex_;
    float fraction = fraction_, smooth = smooth_, a = cachedA_, b = cachedB_;
    if (overdub) retention_ = retention;
    const float inputGain = post_ ? 1.0f : overdub ? 0.5f : liveInput_;
    // The gate applies to playback only: overdub always lets the loop be heard.
    const bool gating = gateMode_ && state_ == 2;
    const float gateTarget = gating && !gateOpen_ ? 0.0f : 1.0f;
    // A window jump fades out like a gate restart, in every state.
    restart = (restart && gating) || jump_;
    bool jumpReady = false;
    float gate = gate_;
    // Grains need room for two of them inside the loop.
    const float width = length / 2 < kGrainFrames ? length / 2 : kGrainFrames;
    const bool grains = kGrains && width >= 4;
    const int32_t lengthFx = (int32_t)length << kFxBits;
    const int32_t widthFx = (int32_t)(width * kFxOne), halfFx = widthFx / 2;
    const float gainScale = 2.0f / widthFx;
    // Per computed grain sample, i.e. every second audio sample.
    const int32_t grainRate = (int32_t)lroundf((kReverse ? 1.0f - pitch : pitch - 1.0f) * 2 * step * kFxOne);
    const int32_t headStep = (int32_t)lroundf((kReverse ? -pitch : pitch) * 2 * step * kFxOne);
    bool grainSkip = grainSkip_;
    float grainLast = grainLast_;
    int32_t grain = grain_ >= 0 && grain_ < widthFx ? grain_ : 0;
    Head head0 = heads_[0], head1 = heads_[1];
    if (grains && !headsPlaced_) {
      int32_t second = grain + halfFx;
      if (second >= widthFx) second -= widthFx;
      place(head0, position, fraction, grain, lengthFx);
      place(head1, position, fraction, second, lengthFx);
      head0.cached = head1.cached = kNoCache;
    }
    headsPlaced_ = grains;
    for (; k < n; ++k) {
      // With grains only the heads read; the play position just keeps time.
      if (!grains && position != cached) {
        size_t next = position + 1 == length ? 0 : position + 1;
        a = decode(at(begin + position));
        b = decode(at(begin + next));
        cached = position;
      }
      float sample;
      const bool grainTick = grains && !grainSkip;
      if (grainTick) {
        float gain = 1.0f - fabsf(grain * gainScale - 1.0f);
        float latest = gain * read(head0, begin, length, fadeScale, fadeLength) +
                       (1.0f - gain) * read(head1, begin, length, fadeScale, fadeLength);
        sample = 0.5f * (grainLast + latest);
        grainLast = latest;
      } else if (grains) {
        sample = grainLast;
      } else {
        sample = a + (b - a) * fraction;
        // Short edge fade suppresses discontinuities without changing loop duration.
        size_t back = length - position;
        if (position < 9 || back <= 9) {
          float edge = position < back ? position + fraction : back - fraction;
          if (edge < fadeLength) sample *= edge * fadeScale;
        }
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
      if (grains) grainSkip = !grainSkip;
      if (grainTick) {
        // Heads move at the pitch rate; the one whose offset wraps (silent at
        // that moment) jumps back next to the new play position.
        advance(head0, headStep, lengthFx);
        advance(head1, headStep, lengthFx);
        int32_t next = grain + grainRate;
        bool crossedHalf = (grain < halfFx) != (next < halfFx);
        if (next >= widthFx || next < 0) {
          next += next < 0 ? widthFx : -widthFx;
          place(head0, position, fraction, next, lengthFx);
        }
        if (crossedHalf) {
          int32_t second = next + halfFx;
          if (second >= widthFx) second -= widthFx;
          place(head1, position, fraction, second, lengthFx);
        }
        grain = next;
      }
      smooth += 0.02f * (level - smooth);
      gate += kGateRate * ((restart ? 0.0f : gateTarget) - gate);
      // During overdub (and playback with live input on), let the player hear the
      // input as well as the existing loop. -6 dB keeps their combined drive controlled.
      output[k] = sample * smooth * gate + input[k] * inputGain;
      if (restart && !jumpReady && gate < kGateFloor && jump_) {
        // Silent now: the next block enters the new window; stay silent until then.
        jumpReady = true;
      } else if (restart && !jumpReady && gate < kGateFloor) {
        // Silent now: jump to the loop start (the end in reverse) and re-place the grains.
        restart = false;
        position = kReverse ? length - 1 : 0;
        fraction = kReverse ? 0.9999f : 0;
        cached = kNoCache;
        if (grains) {
          grain = 0;
          place(head0, position, fraction, 0, lengthFx);
          place(head1, position, fraction, halfFx, lengthFx);
          head0.cached = head1.cached = kNoCache;
        }
      }
    }
    restart_ = restart && !jumpReady;
    jumpReady_ = jumpReady;
    position_ = position;
    fraction_ = fraction;
    smooth_ = smooth;
    gate_ = gate;
    grain_ = grain;
    grainSkip_ = grainSkip;
    grainLast_ = grainLast;
    heads_[0] = head0;
    heads_[1] = head1;
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
  // Absolute frame, for display (the window offset plus the relative position).
  size_t position() const { return begin_ + position_; }
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
  // Audio task only: hear the live input at -6 dB during playback too.
  void setLiveInput(bool on) { liveInput_ = on ? 0.5f : 0; }
  // Audio task only: the loop sits after the resonator, so the input passes at unity.
  void setPost(bool on) { post_ = on; }
  // Audio task only: gate mode and whether a key or note holds the gate open.
  void setGate(bool mode, bool open) { gateMode_ = mode; gateOpen_ = open; }
  // Audio task only: in gate mode, restart playback from the loop start.
  void retrigger() { restart_ = true; }
  // Audio task only: the playback window as shares of the recording (0-1); it is
  // applied at the next block, at least kMinSpan frames long.
  void setWindow(float start, float end) {
    windowStart_ = start < 0 ? 0 : start > 1 ? 1 : start;
    windowEnd_ = end < 0 ? 0 : end > 1 ? 1 : end;
  }
  void command(int value) {
    if (!capacity_) return;
    restart_ = false;
    cachedIndex_ = kNoCache;
    headsPlaced_ = false;
    jump_ = jumpReady_ = false;
    if (value == 4) {
      length_ = 0; position_ = 0; fraction_ = 0; state_ = 0;
      begin_ = span_ = 0;
      recordSum_ = overdubInputSum_ = overdubFeedbackSum_ = 0;
      recordCount_ = overdubCount_ = 0;
    }
    else if (value == 1) {
      if (state_ != 1) {
        length_ = 0; position_ = 0; fraction_ = 0; recordSum_ = 0; recordCount_ = 0;
        begin_ = span_ = 0;
        overdubInputSum_ = overdubFeedbackSum_ = 0; overdubCount_ = 0;
      }
      state_ = 1;
    }
    else if (value >= 0 && value <= 3) {
      if (state_ == 1) finishRecording();
      if (state_ == 3) finishOverdub();
      state_ = (value >= 2 && !length_) ? 0 : value;
      if (state_ >= 2) {
        updateWindow(false, false);
        if (position_ >= span_) { position_ = 0; fraction_ = 0; }
      }
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
  // `pitch` other than 1 shifts the pitch by that ratio with grains, independently
  // of `step` (the stretch mode plays at step 1/rateDivision()).
  // The grain path is a separate instantiation so the plain one stays lean.
  void process(const float* input, float* output, int n, float step, float level,
               float retention, const float* feedback, float cross, float pitch = 1) {
    if (pitch < 0.9999f || pitch > 1.0001f)
      processBlock<false, true>(input, output, n, step, level, retention, feedback, cross, pitch);
    else
      processBlock<false, false>(input, output, n, step, level, retention, feedback, cross, pitch);
  }
  void processReverse(const float* input, float* output, int n, float step, float level,
                      float retention, const float* feedback, float cross, float pitch = 1) {
    if (pitch < 0.9999f || pitch > 1.0001f)
      processBlock<true, true>(input, output, n, step, level, retention, feedback, cross, pitch);
    else
      processBlock<true, false>(input, output, n, step, level, retention, feedback, cross, pitch);
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
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
