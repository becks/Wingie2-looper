// Architecture extension; include after Faust dsp/decorator_dsp definitions.
#pragma once
#include "dual_looper.h"
#include "level_meter.h"
#include "speed_steps.h"
#include <atomic>
#include <Arduino.h>
#include "esp_heap_caps.h"
// Set to 1 to dump the heap map at boot when retuning the allocation.
#ifndef LOOPER_MEM_DEBUG
#define LOOPER_MEM_DEBUG 0
#endif
// Audio-task CPU cycles per block, published about every 2 s for the serial
// log; build with -DLOOPER_PROFILE=1 to measure overdub headroom.
#ifndef LOOPER_PROFILE
#define LOOPER_PROFILE 0
#endif
#if LOOPER_PROFILE
#include <xtensa/hal.h>
#endif
class LooperDSP : public decorator_dsp {
  wingie_loop::Loop loop_[2];
  // Chunk pairs, one per channel, so both loops always get the same length.
  uint8_t* chunks_[2][wingie_loop::Loop::kMaxChunks] = {};
  size_t chunkCount_ = 0;
  std::atomic<int> command_[2] {{0}, {0}};
  std::atomic<float> speed_[2] {{1}, {1}}, level_[2] {{1}, {1}}, retention_[2] {{0.8f}, {0.8f}}, cross_[2] {{0}, {0}};
  std::atomic<int> speedMode_[2] {{0}, {0}};
  std::atomic<bool> reverse_[2] {{false}, {false}}, filled_[2] {{false}, {false}}, wrapped_[2] {{false}, {false}};
  std::atomic<bool> liveInput_ {false};
  float feedback_[2][64] = {};
  wingie_loop::LevelMeter meter_;
  static constexpr uint32_t kCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#if LOOPER_PROFILE
  static constexpr uint32_t kProfileBlocks = 1378;  // about 2 s of 64-sample blocks
  // Written only by the audio task; published through profOut_/profSeq_.
  uint32_t profBlocks_ = 0, profSumTotal_ = 0, profMaxTotal_ = 0;
  uint32_t profSumLoop_ = 0, profMaxLoop_ = 0, profBlockLoop_ = 0;
  std::atomic<uint32_t> profOut_[6] {};
  std::atomic<uint32_t> profSeq_ {0};
  uint32_t profSeen_ = 0;  // reader side only
#endif
  // The audio task (4096 B stack) and the control task (7168 B, created after
  // dsp.start()) need contiguous stacks after allocation: one block of this size is
  // held while the chunks are taken, then freed for them. Margin covers the TCBs.
  static constexpr size_t kTaskHole = 4096 + 7168 + 1024;
  // Heap left besides that hole for Preferences, the serial protocol and the
  // driver's small runtime allocations (measured ~3 KB), with margin for untested
  // paths. No PSRAM assumption.
  static constexpr size_t kHeapReserve = 8192;
  // Fallback when the hole cannot be held: the old contiguity rule.
  static constexpr size_t kMinLargestBlock = 16384;
  void releaseLastPair() {
    --chunkCount_;
    for (int ch=0; ch<2; ++ch) {
      heap_caps_free(chunks_[ch][chunkCount_]);
      chunks_[ch][chunkCount_] = nullptr;
    }
  }
public:
  explicit LooperDSP(dsp* inner) : decorator_dsp(inner) {}
  ~LooperDSP() { while (chunkCount_) releaseLastPair(); }
  bool allocate() {
    if (chunkCount_) return true;
    const size_t chunk = wingie_loop::Loop::kChunkSize;
    Serial.printf("LOOPER_MEM before free=%u largest=%u min_free=%u reserve=%u min_largest=%u chunk=%u\n",
                  (unsigned)heap_caps_get_free_size(kCaps),
                  (unsigned)heap_caps_get_largest_free_block(kCaps),
                  (unsigned)heap_caps_get_minimum_free_size(kCaps),
                  (unsigned)kHeapReserve, (unsigned)kMinLargestBlock, (unsigned)chunk);
#if LOOPER_MEM_DEBUG
    heap_caps_print_heap_info(kCaps);
#endif
    void* hole = heap_caps_malloc(kTaskHole, kCaps);
    while (chunkCount_ < wingie_loop::Loop::kMaxChunks &&
           heap_caps_get_free_size(kCaps) >= kHeapReserve + 2*chunk) {
      uint8_t* a = (uint8_t*)heap_caps_malloc(chunk, kCaps);
      uint8_t* b = a ? (uint8_t*)heap_caps_malloc(chunk, kCaps) : nullptr;
      if (!b) {
        heap_caps_free(a);
        break;
      }
      chunks_[0][chunkCount_] = a;
      chunks_[1][chunkCount_] = b;
      ++chunkCount_;
    }
    if (hole) {
      heap_caps_free(hole);
    } else {
      while (chunkCount_ && heap_caps_get_largest_free_block(kCaps) < kMinLargestBlock)
        releaseLastPair();
    }
    if (!chunkCount_) {
      Serial.printf("LOOPER_MEM allocation_failed reason=insufficient_heap\n");
      return false;
    }
    for (int ch=0; ch<2; ++ch) loop_[ch].attach(chunks_[ch], chunkCount_);
    size_t frames = chunkCount_ * chunk;
    Serial.printf("LOOPER_MEM allocated=%u chunks_per_channel=%u frames_per_channel=%u seconds_per_channel=%.3f after_free=%u after_largest=%u\n",
                  (unsigned)(2*frames), (unsigned)chunkCount_, (unsigned)frames,
                  frames * wingie_loop::Loop::rateDivision() / (float)getSampleRate(),
                  (unsigned)heap_caps_get_free_size(kCaps),
                  (unsigned)heap_caps_get_largest_free_block(kCaps));
#if LOOPER_MEM_DEBUG
    heap_caps_print_heap_info(kCaps);
#endif
    return true;
  }
  // Loop Monitor reads, from the serial task while audio runs (display only).
  void view(int ch, wingie_loop::LoopView& out) const {
    if (ch < 0 || ch > 1) return;
    const wingie_loop::Loop& loop = loop_[ch];
    out.state = loop.state();
    out.length = loop.length();
    out.position = loop.position();
    out.capacity = loop.capacity();
    out.speed = std::max(0.25f, std::min(2.0f, speed_[ch].load()));
    out.level = level_[ch].load();
    out.reverse = reverse_[ch].load();
    out.retention = retention_[ch].load();
    out.speedMode = speedMode_[ch].load();
  }
  void peaks(int ch, uint8_t* out, size_t buckets) const {
    if (ch >= 0 && ch < 2) loop_[ch].peaks(out, buckets);
  }
  size_t readLevels(uint32_t& from, uint8_t out[][wingie_loop::LevelMeter::kSize], size_t max) const {
    return meter_.read(from, out, max);
  }
  // Share of the loop kept on each overdub pass ("sound on sound"), 0-1.
  void setRetention(int ch, float value) {
    if (ch >= 0 && ch < 2) retention_[ch].store(std::max(0.0f, std::min(1.0f, value)));
  }
  // Exact speed (pitch) ratio, for the interval steps; CC 81 stays continuous.
  void setSpeed(int ch, float value) {
    if (ch >= 0 && ch < 2)
      speed_[ch].store(std::max(wingie_loop::kSpeedMin, std::min(wingie_loop::kSpeedMax, value)));
  }
  // What the speed acts on: varispeed, pitch only or tempo only (SpeedMode).
  void setSpeedMode(int ch, int mode) {
    if (ch >= 0 && ch < 2 && mode >= 0 && mode < wingie_loop::kSpeedModeCount) speedMode_[ch].store(mode);
  }
  // Both sides: keep the live input audible (-6 dB) while a loop plays.
  void setLiveInput(bool on) { liveInput_.store(on); }
  bool takeFilled(int ch) { return ch >= 0 && ch < 2 && filled_[ch].exchange(false); }
  bool takeWrapped(int ch) { return ch >= 0 && ch < 2 && wrapped_[ch].exchange(false); }
  void control(int ch, int cc, int value) {
    if (ch < 0 || ch > 1) return;
    switch(cc) {
      case 80: command_[ch].store(value < 5 ? value : 0); break;
      case 81:
        speed_[ch].store(value <= 64 ? powf(2, (value-64)/64.0f)
                                     : powf(2, (value-64)/63.0f));
        break;
      case 82: level_[ch].store(value/127.0f); break;
      case 83: setRetention(ch, value/127.0f); break;
      case 87: setSpeedMode(ch, wingie_loop::speedModeFromCC(value)); break;
      case 86: setSpeed(ch, wingie_loop::kSpeedSteps[wingie_loop::speedStepFromCC(value)]); break;
      case 84: cross_[ch].store(0.25f * value/127.0f); break;
      case 85: reverse_[ch].store(value >= 64); break;
    }
  }
  // Fills out[] with average/peak cycles per block (total, looper only) and the
  // two loop states; true once per published window.
  bool takeProfile(uint32_t out[6]) {
#if LOOPER_PROFILE
    uint32_t seq = profSeq_.load();
    if (seq == profSeen_) return false;
    profSeen_ = seq;
    for (int i=0; i<6; ++i) out[i] = profOut_[i].load();
    return true;
#else
    (void)out;
    return false;
#endif
  }
  void compute(int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs) override {
#if LOOPER_PROFILE
    uint32_t start = xthal_get_ccount();
    profBlockLoop_ = 0;
    computeBlock(count, inputs, outputs);
    uint32_t total = xthal_get_ccount() - start;
    profSumTotal_ += total;
    profSumLoop_ += profBlockLoop_;
    if (total > profMaxTotal_) profMaxTotal_ = total;
    if (profBlockLoop_ > profMaxLoop_) profMaxLoop_ = profBlockLoop_;
    if (++profBlocks_ == kProfileBlocks) {
      profOut_[0].store(profSumTotal_ / kProfileBlocks);
      profOut_[1].store(profMaxTotal_);
      profOut_[2].store(profSumLoop_ / kProfileBlocks);
      profOut_[3].store(profMaxLoop_);
      profOut_[4].store(loop_[0].state());
      profOut_[5].store(loop_[1].state());
      profSeq_.fetch_add(1);
      profBlocks_ = profSumTotal_ = profMaxTotal_ = profSumLoop_ = profMaxLoop_ = 0;
    }
#else
    computeBlock(count, inputs, outputs);
#endif
    // Raw codec input and final output, for the Loop Monitor meters.
    const float* signals[4] = {inputs[0], inputs[1], outputs[0], outputs[1]};
    meter_.addBlock(signals, count);
  }
private:
  void computeBlock(int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs) {
    float speed[2], level[2], retention[2], cross[2];
    bool reverse[2];
    int mode[2];
    const bool liveInput = liveInput_.load();
    for (int ch=0; ch<2; ++ch) {
      int command = command_[ch].exchange(-1);
      if (command >= 0) loop_[ch].command(command);
      speed[ch]=std::max(0.25f, std::min(2.0f, speed_[ch].load()));
      reverse[ch]=reverse_[ch].load();
      mode[ch]=speedMode_[ch].load();
      level[ch]=level_[ch].load();
      retention[ch]=retention_[ch].load(); cross[ch]=cross_[ch].load();
      loop_[ch].setLiveInput(liveInput);
    }
    bool active[2] = {loop_[0].active(), loop_[1].active()};
    if (!active[0] && !active[1]) {
      fDSP->compute(count, inputs, outputs);
      return;
    }
    bool captureFeedback = cross[0] > 0 || cross[1] > 0;
    // Driver buffers are 64 samples; chunking also supports other block sizes.
    for (int offset=0; offset<count; offset+=64) {
      int n=std::min(64, count-offset);
      FAUSTFLOAT storage[2][64];
      FAUSTFLOAT *in[2]={active[0] ? storage[0] : inputs[0]+offset,
                        active[1] ? storage[1] : inputs[1]+offset};
      FAUSTFLOAT *out[2]={outputs[0]+offset,outputs[1]+offset};
#if LOOPER_PROFILE
      uint32_t loopStart = xthal_get_ccount();
#endif
      for (int ch=0; ch<2; ++ch) {
        if (!active[ch]) continue;
        // Pitch mode keeps the play position at 1x and shifts the grains; stretch
        // moves it at the speed while the grains keep the recorded pitch.
        float tempo, pitch;
        wingie_loop::speedModeRates(mode[ch], speed[ch], tempo, pitch);
        float step = tempo / wingie_loop::Loop::rateDivision();
        const float* feedback = captureFeedback ? feedback_[1-ch] : nullptr;
        if (reverse[ch])
          loop_[ch].processReverse(inputs[ch]+offset, in[ch], n, step, level[ch], retention[ch], feedback, cross[ch], pitch);
        else
          loop_[ch].process(inputs[ch]+offset, in[ch], n, step, level[ch], retention[ch], feedback, cross[ch], pitch);
      }
#if LOOPER_PROFILE
      profBlockLoop_ += xthal_get_ccount() - loopStart;
#endif
      fDSP->compute(n,in,out);
      if (captureFeedback) {
        for (int i=0; i<n; ++i) for (int ch=0; ch<2; ++ch)
          feedback_[ch][i]=wingie_loop::Loop::limit(out[ch][i]);
      }
    }
    for (int ch=0; ch<2; ++ch) {
      if (loop_[ch].takeFilled()) filled_[ch].store(true);
      if (loop_[ch].takeWrapped()) wrapped_[ch].store(true);
    }
  }
};
