// Architecture extension; include after Faust dsp/decorator_dsp definitions.
#pragma once
#include "dual_looper.h"
#include <atomic>
#include <Arduino.h>
#include "esp_heap_caps.h"
// Set to 1 to dump the heap map at boot when retuning the allocation.
#ifndef LOOPER_MEM_DEBUG
#define LOOPER_MEM_DEBUG 0
#endif
class LooperDSP : public decorator_dsp {
  wingie_loop::Loop loop_[2];
  // Chunk pairs, one per channel, so both loops always get the same length.
  uint8_t* chunks_[2][wingie_loop::Loop::kMaxChunks] = {};
  size_t chunkCount_ = 0;
  std::atomic<int> command_[2] {{0}, {0}};
  std::atomic<float> speed_[2] {{1}, {1}}, level_[2] {{1}, {1}}, retention_[2] {{0.9f}, {0.9f}}, cross_[2] {{0}, {0}};
  std::atomic<bool> reverse_[2] {{false}, {false}}, filled_[2] {{false}, {false}};
  float feedback_[2][64] = {};
  static constexpr uint32_t kCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  // Heap left after allocation for the audio driver, its task, the control task
  // (created after dsp.start()), Preferences and the serial protocol. No PSRAM assumption.
  static constexpr size_t kHeapReserve = 32768;
  // Contiguous room for task stacks created after allocation (control uses 10000 B).
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
    while (chunkCount_ && heap_caps_get_largest_free_block(kCaps) < kMinLargestBlock)
      releaseLastPair();
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
  bool takeFilled(int ch) { return ch >= 0 && ch < 2 && filled_[ch].exchange(false); }
  void control(int ch, int cc, int value) {
    if (ch < 0 || ch > 1) return;
    switch(cc) {
      case 80: command_[ch].store(value < 5 ? value : 0); break;
      case 81:
        speed_[ch].store(value <= 64 ? powf(2, (value-64)/64.0f)
                                     : powf(2, (value-64)/63.0f));
        break;
      case 82: level_[ch].store(value/127.0f); break;
      case 83: retention_[ch].store(value/127.0f); break;
      case 84: cross_[ch].store(0.25f * value/127.0f); break;
      case 85: reverse_[ch].store(value >= 64); break;
    }
  }
  void compute(int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs) override {
    float speed[2], level[2], retention[2], cross[2];
    bool reverse[2];
    for (int ch=0; ch<2; ++ch) {
      int command = command_[ch].exchange(-1);
      if (command >= 0) loop_[ch].command(command);
      speed[ch]=std::max(0.25f, std::min(2.0f, speed_[ch].load()));
      reverse[ch]=reverse_[ch].load();
      level[ch]=level_[ch].load();
      retention[ch]=retention_[ch].load(); cross[ch]=cross_[ch].load();
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
      for (int ch=0; ch<2; ++ch) {
        if (!active[ch]) continue;
        if (reverse[ch]) {
          for (int i=0; i<n; ++i)
            in[ch][i]=loop_[ch].processReverse(inputs[ch][offset+i],speed[ch],level[ch],retention[ch],cross[ch]*feedback_[1-ch][i]);
        } else {
          for (int i=0; i<n; ++i)
            in[ch][i]=loop_[ch].process(inputs[ch][offset+i],speed[ch],level[ch],retention[ch],cross[ch]*feedback_[1-ch][i]);
        }
      }
      fDSP->compute(n,in,out);
      if (captureFeedback) {
        for (int i=0; i<n; ++i) for (int ch=0; ch<2; ++ch)
          feedback_[ch][i]=wingie_loop::Loop::limit(out[ch][i]);
      }
    }
    for (int ch=0; ch<2; ++ch)
      if (loop_[ch].takeFilled()) filled_[ch].store(true);
  }
};
