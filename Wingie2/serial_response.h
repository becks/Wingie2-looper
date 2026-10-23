#ifndef WINGIE2_SERIAL_RESPONSE_H
#define WINGIE2_SERIAL_RESPONSE_H

// 串口配置协议响应编码层（Arduino-free 纯头）。
//
// 与 serial_config_protocol.h 的请求解析相对：本头只把参数化状态编码为协议
// JSON 响应帧。所有 encode* 函数不触碰任何全局状态 / Preferences / DSP 句柄，
// 由 serial_config.ino 读取全局状态（含 device_state.h 快照助手）后填充结构体
// 再调用，保证响应字节与设备状态解耦、可在 host 侧单测。

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config_profiles.h"
#include "serial_config_protocol.h"

namespace wingie_serial {

class JsonResponse {
public:
  char data[kMaxFrameBytes + 1];
  size_t length;
  bool valid;

  JsonResponse() : length(0), valid(true) {
    data[0] = '\0';
  }

  void append(const char *format, ...) __attribute__((format(printf, 2, 3))) {
    if (!valid || length >= kMaxFrameBytes) return;
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(data + length, sizeof(data) - length, format, arguments);
    va_end(arguments);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(data) - length) {
      valid = false;
      return;
    }
    length += static_cast<size_t>(written);
  }
};

// get_settings 单声道参数快照（serial_config.ino 读取 dsp/Mode/threshold 后填充）
struct ChannelSettings {
  int mode;
  float mix;
  float decay;
  float volume;
  float threshold;
  float pan;  // post-resonator pan, 0 hard left .. 1 hard right
};

// get_settings shared 段参数快照
struct SharedSettings {
  float a3Hz;
  int tuning;            // use_alt_tuning ? alt_tuning_index : -1
  float preClipGain;
  float postClipGain;
  int midiLeft;
  int midiRight;
  int midiBoth;
  bool mpeEnabled;
  bool lineInputMono;
  bool looperLatch;  // tap both Mode buttons to lock the looper layer
};

// status 响应快照
struct StatusSnapshot {
  int mode[2];
  int note[2];
  float fundamentalHz[2];
  uint32_t midiRx;
  uint32_t profileRevision;
  uint8_t activeBank[2];
  uint32_t caveRevision[2][wingie_config::kCaveBankCount];
};

// get_controls 响应快照（controlActivity 与 midi_rx_count 的整块拷贝）
struct ControlCountsSnapshot {
  uint32_t midiRx;
  int note[2];
  uint16_t key[2][12];
  uint16_t modeButton[2];
  uint16_t octButton[2][2];
  uint16_t sourceSwitch;
  uint16_t pot[3];
};

// Loop Monitor: waveform buckets per loop_wave frame (two hex digits each) and
// note events per loop_notes page, both sized to stay under kMaxFrameBytes.
static const size_t kLoopWaveBuckets = 384;
static const size_t kLoopNotesPage = 48;
// Level entries (input L/R, output L/R) per loop_status frame.
static const size_t kLoopLevelEntries = 40;

// One side of a loop_status frame: audio loop, note loop and panel state.
struct LoopSideSnapshot {
  int audioState;
  uint32_t length;
  uint32_t position;
  uint32_t capacity;
  float speed;
  float level;
  bool reverse;
  float retention;  // sound on sound: share kept per overdub pass
  int speedMode;    // 0 varispeed, 1 pitch only, 2 tempo only
  int notesState;
  uint16_t noteCount;
  uint32_t notesLengthMs;
  uint32_t notesPositionMs;
  uint32_t notesRevision;
  int mode;
  int octave;
  int note;
  uint16_t keys;  // bit i set while panel key i is held
  float mix;
  float decay;
  float volume;
  float pan;
};

struct LoopStatusSnapshot {
  uint32_t ms;
  uint32_t frameRate;
  bool lineInputMono;  // toggled by holding both C keys at boot
  int looperLatch;     // 0 off, 1 on, 2 on with the looper layer locked
  float threshold[2];  // sequencer threshold per side
  float preClipGain;
  float postClipGain;
  bool lineMonoActive;  // mono applies only while the source is line
  bool sourceLine;
  // Encoded peaks (see LevelMeter::encode) since the previous frame.
  uint8_t levels[4][kLoopLevelEntries];
  size_t levelCount;
  LoopSideSnapshot side[2];
};

inline void appendRatioArray(JsonResponse &response, const float *ratios) {
  response.append("[");
  for (uint8_t index = 0; index < wingie_config::kRatioCount; index++) {
    response.append(index ? ",%.3f" : "%.3f", ratios[index]);
  }
  response.append("]");
}

inline void appendCaveFrequencyArray(JsonResponse &response, const float *frequencies) {
  response.append("[");
  for (uint8_t index = 0; index < wingie_config::kRatioCount; index++) {
    response.append(index ? ",%.2f" : "%.2f", frequencies[index]);
  }
  response.append("]");
}

inline void appendCaveMuteArray(JsonResponse &response, const bool *mute) {
  response.append("[");
  for (uint8_t index = 0; index < wingie_config::kRatioCount; index++) {
    response.append(index ? ",%s" : "%s", mute[index] ? "true" : "false");
  }
  response.append("]");
}

inline void appendControlCounterArray(JsonResponse &response, const volatile uint16_t *values,
                                      size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (i) response.append(",");
    response.append("%u", static_cast<unsigned>(values[i]));
  }
}

inline void encodeError(JsonResponse &response, uint32_t id, const char *code,
                        const char *field, const char *message) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":false,\"error\":{\"code\":\"%s\"",
                  static_cast<unsigned long>(id), code);
  if (field) response.append(",\"field\":\"%s\"", field);
  if (message) response.append(",\"message\":\"%s\"", message);
  response.append("}}");
}

inline void appendChannelSettings(JsonResponse &response, const ChannelSettings &settings) {
  response.append("{\"mode\":%d,\"mix\":%.4f,\"decay\":%.4f,\"volume\":%.4f,\"threshold\":%.4f,"
                  "\"pan\":%.4f}",
                  settings.mode, settings.mix, settings.decay, settings.volume, settings.threshold,
                  settings.pan);
}

// 性能参数 limits（单一真值在 config_profiles.h；网页按 name 消费，缺失时回退页面默认）
inline void appendSettingsLimits(JsonResponse &response) {
  response.append(
      ",\"limits\":{\"mode\":[%g,%g,1],"
      "\"threshold\":[%g,%g,%g],"
      "\"pan\":[%g,%g,%g],"
      "\"a3_hz\":[%g,%g,%g],"
      "\"tuning\":[%g,%g,1],"
      "\"pre_clip_gain\":[%g,%g,%g],"
      "\"post_clip_gain\":[%g,%g,%g],"
      "\"midi_left\":[%g,%g,1],"
      "\"midi_right\":[%g,%g,1],"
      "\"midi_both\":[%g,%g,1],"
      "\"mpe_enabled\":[%g,%g,1],"
      "\"line_input_mono\":[%g,%g,1],"
      "\"looper_latch\":[%g,%g,1]}",
      static_cast<float>(wingie_config::kModeMin), static_cast<float>(wingie_config::kModeMax),
      wingie_config::kThresholdMin, wingie_config::kThresholdMax, wingie_config::kThresholdStep,
      wingie_config::kPanMin, wingie_config::kPanMax, wingie_config::kPanStep,
      wingie_config::kA3FrequencyMin, wingie_config::kA3FrequencyMax, wingie_config::kA3FrequencyStep,
      static_cast<float>(wingie_config::kTuningMin), static_cast<float>(wingie_config::kTuningMax),
      wingie_config::kPreClipGainMin, wingie_config::kClipGainMax, wingie_config::kThresholdStep,
      wingie_config::kPostClipGainMin, wingie_config::kClipGainMax, wingie_config::kClipGainStep,
      static_cast<float>(wingie_config::kMidiChannelMin), static_cast<float>(wingie_config::kMidiChannelMax),
      static_cast<float>(wingie_config::kMidiChannelMin), static_cast<float>(wingie_config::kMidiChannelMax),
      static_cast<float>(wingie_config::kMidiChannelMin), static_cast<float>(wingie_config::kMidiChannelMax),
      static_cast<float>(wingie_config::kMpeEnabledMin), static_cast<float>(wingie_config::kMpeEnabledMax),
      static_cast<float>(wingie_config::kLineInputMonoMin), static_cast<float>(wingie_config::kLineInputMonoMax),
      static_cast<float>(wingie_config::kLooperLatchMin), static_cast<float>(wingie_config::kLooperLatchMax));
}

inline void encodeHello(JsonResponse &response, uint32_t id, const char *firmware) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":true,\"op\":\"hello\","
                  "\"device\":\"Wingie2\","
                  "\"firmware\":\"%s\","
                  "\"capabilities\":[\"settings\",\"ratio_mode\",\"cave_config\",\"mpe\",\"speed_steps\",\"speed_modes\"],"
                  "\"config_schema\":6,\"transport\":{\"baud\":115200,\"max_frame\":%u}}",
                  static_cast<unsigned long>(id), firmware,
                  static_cast<unsigned>(kMaxFrameBytes));
}

// dirty 只反映 general settings（midi/调律/增益/阈值/模式/MPE/线路单声道/looper latch）；
// dirtyAll 反映全量配置（另含 ratio 与 cave），供只读 get_settings 的消费方判断是否真正全部落盘。
inline void encodeSettings(JsonResponse &response, uint32_t id, bool sourceLine, bool dirty,
                           bool dirtyAll,
                           const ChannelSettings &left, const ChannelSettings &right,
                           const SharedSettings &shared) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":true,\"op\":\"get_settings\","
                  "\"source\":\"%s\",\"dirty\":%s,\"dirty_all\":%s,\"left\":",
                  static_cast<unsigned long>(id), sourceLine ? "line" : "mic",
                  dirty ? "true" : "false", dirtyAll ? "true" : "false");
  appendChannelSettings(response, left);
  response.append(",\"right\":");
  appendChannelSettings(response, right);
  response.append(",\"shared\":{\"a3_hz\":%.2f,\"tuning\":%d,"
                  "\"pre_clip_gain\":%.4f,\"post_clip_gain\":%.4f,"
                  "\"midi\":{\"left\":%d,\"right\":%d,\"both\":%d},"
                  "\"mpe_enabled\":%s,\"line_input_mono\":%s,\"looper_latch\":%s}",
                  shared.a3Hz, shared.tuning,
                  shared.preClipGain, shared.postClipGain,
                  shared.midiLeft, shared.midiRight, shared.midiBoth,
                  shared.mpeEnabled ? "true" : "false",
                  shared.lineInputMono ? "true" : "false",
                  shared.looperLatch ? "true" : "false");
  appendSettingsLimits(response);
  response.append("}");
}

inline void encodeRatioProfile(JsonResponse &response, uint32_t id, const float *ratios,
                               uint32_t revision, bool dirty) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":true,\"op\":\"get\",\"profile\":{\"ratios\":",
                  static_cast<unsigned long>(id));
  appendRatioArray(response, ratios);
  response.append(",\"revision\":%lu,\"dirty\":%s},\"factory_profile\":{\"ratios\":",
                  static_cast<unsigned long>(revision), dirty ? "true" : "false");
  appendRatioArray(response, wingie_config::kDefaultRatios);
  response.append("},\"limits\":{\"min\":%.3f,\"max\":%.3f,\"step\":%.3f,"
                  "\"frequency_min\":%u,\"frequency_max\":%u}}",
                  wingie_config::kRatioMin, wingie_config::kRatioMax, wingie_config::kRatioStep,
                  wingie_config::kRatioFrequencyMin, wingie_config::kRatioFrequencyMax);
}

inline void encodeCaveBank(JsonResponse &response, uint32_t id, const char *side, uint8_t bank,
                           bool active, const float *frequencies, const bool *mutes,
                           uint32_t revision, bool dirty) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":true,\"op\":\"get_cave\","
                  "\"side\":\"%s\",\"bank\":%u,\"active\":%s,\"frequencies\":",
                  static_cast<unsigned long>(id), side, static_cast<unsigned>(bank),
                  active ? "true" : "false");
  appendCaveFrequencyArray(response, frequencies);
  response.append(",\"mute\":");
  appendCaveMuteArray(response, mutes);
  response.append(",\"revision\":%lu,\"dirty\":%s,\"limits\":{\"min\":%.2f,\"max\":%.2f,\"step\":%.2f}}",
                  static_cast<unsigned long>(revision), dirty ? "true" : "false",
                  wingie_config::kCaveFrequencyMin, wingie_config::kCaveFrequencyMax,
                  wingie_config::kCaveFrequencyStep);
}

inline void encodeStatus(JsonResponse &response, uint32_t id, const StatusSnapshot &snapshot) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":true,\"op\":\"status\","
                  "\"mode\":{\"left\":%d,\"right\":%d},"
                  "\"note\":{\"left\":%d,\"right\":%d},"
                  "\"fundamental_hz\":{\"left\":%.3f,\"right\":%.3f},"
                  "\"midi_rx\":%lu,"
                  "\"profile_revision\":%lu,\"cave_active_bank\":{\"left\":%u,\"right\":%u},"
                  "\"cave_revision\":{\"left\":[%lu,%lu,%lu],\"right\":[%lu,%lu,%lu]}}",
                  static_cast<unsigned long>(id), snapshot.mode[0], snapshot.mode[1],
                  snapshot.note[0], snapshot.note[1],
                  snapshot.fundamentalHz[0], snapshot.fundamentalHz[1],
                  static_cast<unsigned long>(snapshot.midiRx),
                  static_cast<unsigned long>(snapshot.profileRevision),
                  static_cast<unsigned>(snapshot.activeBank[0]),
                  static_cast<unsigned>(snapshot.activeBank[1]),
                  static_cast<unsigned long>(snapshot.caveRevision[0][0]),
                  static_cast<unsigned long>(snapshot.caveRevision[0][1]),
                  static_cast<unsigned long>(snapshot.caveRevision[0][2]),
                  static_cast<unsigned long>(snapshot.caveRevision[1][0]),
                  static_cast<unsigned long>(snapshot.caveRevision[1][1]),
                  static_cast<unsigned long>(snapshot.caveRevision[1][2]));
}

inline void encodeControlCounts(JsonResponse &response, uint32_t id,
                                const ControlCountsSnapshot &snapshot) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":true,\"op\":\"get_controls\","
                  "\"midi_rx\":%lu,\"note\":{\"left\":%d,\"right\":%d},"
                  "\"counts\":{\"key\":{\"left\":[",
                  static_cast<unsigned long>(id), static_cast<unsigned long>(snapshot.midiRx),
                  snapshot.note[0], snapshot.note[1]);
  appendControlCounterArray(response, snapshot.key[0], 12);
  response.append("],\"right\":[");
  appendControlCounterArray(response, snapshot.key[1], 12);
  response.append("]},\"mode_button\":[");
  appendControlCounterArray(response, snapshot.modeButton, 2);
  response.append("],\"oct_button\":{\"left\":[");
  appendControlCounterArray(response, snapshot.octButton[0], 2);
  response.append("],\"right\":[");
  appendControlCounterArray(response, snapshot.octButton[1], 2);
  response.append("]},\"source_switch\":%u,\"pot\":[",
                  static_cast<unsigned>(snapshot.sourceSwitch));
  appendControlCounterArray(response, snapshot.pot, 3);
  response.append("]}}");
}

// Compact on purpose: the monitor polls it several times a second over 115200 baud.
// a: [state, length, position, capacity, speed, level, reverse, sos, speed mode] (frames)
// n: [state, events, length_ms, position_ms, revision]
// x: [mix, decay, volume] as the DSP currently has them, then the side's pan
// lv: input L, input R, output L, output R peak entries since the last frame, hex
inline void encodeLoopStatus(JsonResponse &response, uint32_t id, const LoopStatusSnapshot &snapshot) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":true,\"op\":\"loop_status\","
                  "\"ms\":%lu,\"fps\":%lu,\"mono\":%d,\"latch\":%d,"
                  "\"t\":[%.4f,%.4f],\"g\":[%.4f,%.4f],\"sides\":[",
                  static_cast<unsigned long>(id), static_cast<unsigned long>(snapshot.ms),
                  static_cast<unsigned long>(snapshot.frameRate), snapshot.lineInputMono ? 1 : 0,
                  snapshot.looperLatch,
                  snapshot.threshold[0], snapshot.threshold[1], snapshot.preClipGain,
                  snapshot.postClipGain);
  for (int ch = 0; ch < 2; ch++) {
    const LoopSideSnapshot &side = snapshot.side[ch];
    response.append("%s{\"a\":[%d,%lu,%lu,%lu,%.3f,%.3f,%d,%.3f,%d],\"n\":[%d,%u,%lu,%lu,%lu],"
                    "\"m\":%d,\"o\":%d,\"p\":%d,\"k\":%u,\"x\":[%.3f,%.3f,%.3f,%.3f]}",
                    ch ? "," : "", side.audioState, static_cast<unsigned long>(side.length),
                    static_cast<unsigned long>(side.position), static_cast<unsigned long>(side.capacity),
                    side.speed, side.level, side.reverse ? 1 : 0, side.retention, side.speedMode, side.notesState,
                    static_cast<unsigned>(side.noteCount), static_cast<unsigned long>(side.notesLengthMs),
                    static_cast<unsigned long>(side.notesPositionMs),
                    static_cast<unsigned long>(side.notesRevision), side.mode, side.octave, side.note,
                    static_cast<unsigned>(side.keys), side.mix, side.decay, side.volume, side.pan);
  }
  response.append("],\"mono_active\":%d,\"src\":\"%s\",\"lv\":[",
                  snapshot.lineMonoActive ? 1 : 0, snapshot.sourceLine ? "line" : "mic");
  static const char kHex[] = "0123456789abcdef";
  for (int ch = 0; ch < 4; ch++) {
    response.append(ch ? ",\"" : "\"");
    for (size_t i = 0; i < snapshot.levelCount && i < kLoopLevelEntries && response.valid; i++) {
      if (response.length + 2 >= kMaxFrameBytes) {
        response.valid = false;
        return;
      }
      response.data[response.length++] = kHex[snapshot.levels[ch][i] >> 4];
      response.data[response.length++] = kHex[snapshot.levels[ch][i] & 0x0f];
      response.data[response.length] = '\0';
    }
    response.append("\"");
  }
  response.append("]}");
}

// peaks: one byte (0-127) per bucket, hex encoded.
inline void encodeLoopWave(JsonResponse &response, uint32_t id, const char *side, int state,
                           uint32_t length, uint32_t capacity, const uint8_t *peaks, size_t count) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":true,\"op\":\"loop_wave\",\"side\":\"%s\","
                  "\"state\":%d,\"length\":%lu,\"capacity\":%lu,\"peaks\":\"",
                  static_cast<unsigned long>(id), side, state, static_cast<unsigned long>(length),
                  static_cast<unsigned long>(capacity));
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < count && response.valid; i++) {
    if (response.length + 2 >= kMaxFrameBytes) {
      response.valid = false;
      return;
    }
    response.data[response.length++] = kHex[peaks[i] >> 4];
    response.data[response.length++] = kHex[peaks[i] & 0x0f];
    response.data[response.length] = '\0';
  }
  response.append("\"}");
}

// events: [time_ms, key] pairs starting at `offset`.
inline void encodeLoopNotes(JsonResponse &response, uint32_t id, const char *side, int state,
                            uint16_t total, uint32_t lengthMs, uint32_t revision, uint16_t offset,
                            const uint32_t *times, const uint8_t *keys, size_t count) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":true,\"op\":\"loop_notes\",\"side\":\"%s\","
                  "\"state\":%d,\"count\":%u,\"length_ms\":%lu,\"revision\":%lu,"
                  "\"offset\":%u,\"events\":[",
                  static_cast<unsigned long>(id), side, state, static_cast<unsigned>(total),
                  static_cast<unsigned long>(lengthMs), static_cast<unsigned long>(revision),
                  static_cast<unsigned>(offset));
  for (size_t i = 0; i < count; i++) {
    response.append("%s[%lu,%u]", i ? "," : "", static_cast<unsigned long>(times[i]),
                    static_cast<unsigned>(keys[i]));
  }
  response.append("]}");
}

inline void encodeQueued(JsonResponse &response, uint32_t id, const char *operation,
                         uint32_t revision) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":true,\"op\":\"%s\",\"state\":\"queued\",\"revision\":%lu}",
                  static_cast<unsigned long>(id), operation,
                  static_cast<unsigned long>(revision));
}

inline void encodeParameterResponse(JsonResponse &response, uint32_t id, float value, bool dirty,
                                    bool cavesChanged) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":true,\"op\":\"set_param\","
                  "\"value\":%.4f,\"dirty\":%s,\"caves_changed\":%s}",
                  static_cast<unsigned long>(id), value,
                  dirty ? "true" : "false", cavesChanged ? "true" : "false");
}

inline void encodeSaveOk(JsonResponse &response, uint32_t id) {
  response.append("{\"v\":1,\"id\":%lu,\"ok\":true,\"op\":\"save\",\"state\":\"saved\"}",
                  static_cast<unsigned long>(id));
}

// 响应溢出 fallback 帧：encode* 后 valid==false 时写入。返回写入长度。
inline size_t encodeResponseTooLarge(char *out, size_t capacity) {
  const char *fallback = "{\"v\":1,\"id\":0,\"ok\":false,\"error\":{\"code\":\"response_too_large\"}}";
  const int written = snprintf(out, capacity, "%s", fallback);
  return written < 0 ? 0 : static_cast<size_t>(written);
}

}  // namespace wingie_serial

#endif  // WINGIE2_SERIAL_RESPONSE_H
