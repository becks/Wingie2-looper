// Architecture extension; include after Faust dsp/decorator_dsp definitions.
#pragma once
#include "dual_looper.h"
#include <atomic>
#include <Arduino.h>
#include "esp_heap_caps.h"
class LooperDSP : public decorator_dsp {
  wingie_loop::Loop loop_[2];
  uint8_t* memory_ = nullptr;
  std::atomic<int> command_[2] {{0}, {0}};
  std::atomic<float> speed_[2] {{1}, {1}}, level_[2] {{1}, {1}}, retention_[2] {{0.9f}, {0.9f}}, cross_[2] {{0}, {0}};
  std::atomic<bool> reverse_[2] {{false}, {false}};
  float feedback_[2][64] = {};
public:
  explicit LooperDSP(dsp* inner) : decorator_dsp(inner) {}
  ~LooperDSP() { heap_caps_free(memory_); }
  bool allocate() {
    if (memory_) return true;
    // Reserve heap for the audio task, controls and serial protocol; no PSRAM assumption.
    size_t available = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("LOOPER_MEM before free=%u largest=%u reserve=65536 cap=%u\n",
                  (unsigned)available, (unsigned)largest, (unsigned)(getSampleRate()*8));
    size_t bytes = available > 65536 ? available - 65536 : 0;
    bytes = std::min(bytes, std::min(largest / 2, (size_t)(getSampleRate()*8)));
    bytes &= ~size_t(3);
    if (bytes < 4096) {
      Serial.printf("LOOPER_MEM allocation_failed requested=%u reason=insufficient_heap\n",
                    (unsigned)bytes);
      return false;
    }
    memory_ = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory_) {
      Serial.printf("LOOPER_MEM allocation_failed requested=%u reason=malloc_failed\n",
                    (unsigned)bytes);
      return false;
    }
    size_t frames = bytes / 2;
    for (int ch=0; ch<2; ++ch) loop_[ch].attach(memory_ + ch*frames, frames);
    Serial.printf("LOOPER_MEM allocated=%u frames_per_channel=%u seconds_per_channel=%.3f after_free=%u after_largest=%u\n",
                  (unsigned)bytes, (unsigned)frames,
                  frames * wingie_loop::Loop::rateDivision() / (float)getSampleRate(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return true;
  }
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
  }
};
