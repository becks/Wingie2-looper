// Per-side panel key looper: records key presses with millisecond timing and
// replays them in a loop. Arduino-free so it can be tested on the host.
// GPL-3.0-or-later.
#pragma once
#include <stdint.h>

namespace wingie_loop {

class NoteLoop {
 public:
  enum State : uint8_t { kEmpty, kRecording, kPlaying, kStopped };
  static constexpr uint16_t kCapacity = 256;
  // Event time is packed into 24 bits (about 4.6 hours); keys use 8 bits.
  static constexpr uint32_t kMaxLengthMs = 0xffffff;

  State state() const { return state_; }
  uint16_t count() const { return count_; }
  uint32_t lengthMs() const { return length_; }
  bool recording() const { return state_ == kRecording; }
  // Bumped whenever the recorded events change, so a monitor knows when to re-read them.
  uint32_t revision() const { return revision_; }
  uint32_t eventTime(uint16_t i) const { return i < count_ ? events_[i] >> 8 : 0; }
  uint8_t eventKey(uint16_t i) const { return i < count_ ? (uint8_t)(events_[i] & 0xff) : 0; }
  // Milliseconds into the current recording or playback cycle (0 otherwise).
  uint32_t positionMs(uint32_t now) const {
    if (state_ == kRecording) return now - start_;
    if (state_ != kPlaying || !length_) return 0;
    return (now - cycleStart_) % length_;
  }

  void startRecording(uint32_t now) {
    state_ = kRecording;
    count_ = 0;
    length_ = 0;
    start_ = now;
    ++revision_;
  }

  // Returns true when the event buffer filled up (or the maximum length was
  // reached) and the loop closed itself and started playing.
  bool record(uint32_t now, uint8_t key) {
    if (state_ != kRecording) return false;
    uint32_t elapsed = now - start_;
    if (elapsed >= kMaxLengthMs) {
      finishRecording(now);
      return true;
    }
    events_[count_++] = (elapsed << 8) | key;
    ++revision_;
    if (count_ == kCapacity) {
      finishRecording(now);
      return true;
    }
    return false;
  }

  // Closes the recording at `now` and starts playback from the loop start.
  // A loop without keys or duration becomes empty again.
  void finishRecording(uint32_t now) {
    if (state_ != kRecording) return;
    uint32_t elapsed = now - start_;
    if (!count_) {
      clear();
      return;
    }
    // Keep a key pressed in the closing millisecond inside the loop.
    uint32_t last = events_[count_ - 1] >> 8;
    if (elapsed <= last) elapsed = last + 1;
    if (elapsed > kMaxLengthMs) elapsed = kMaxLengthMs;
    length_ = elapsed;
    ++revision_;
    state_ = kStopped;
    play(now);
  }

  void play(uint32_t now) {
    if (state_ == kEmpty || state_ == kRecording) return;
    state_ = kPlaying;
    cycleStart_ = now;
    cursor_ = 0;
  }

  void stop() {
    if (state_ == kPlaying) state_ = kStopped;
  }

  void clear() {
    state_ = kEmpty;
    count_ = 0;
    length_ = 0;
    cursor_ = 0;
    ++revision_;
  }

  // Call repeatedly until it returns false: yields each key that is due at `now`.
  bool next(uint32_t now, uint8_t& key) {
    if (state_ != kPlaying) return false;
    for (;;) {
      uint32_t position = now - cycleStart_;
      if (cursor_ < count_) {
        uint32_t event = events_[cursor_];
        if ((event >> 8) <= position) {
          ++cursor_;
          key = (uint8_t)(event & 0xff);
          return true;
        }
      }
      if (position < length_) return false;
      // Cycle finished (every event lies inside the loop): wrap to the start.
      cycleStart_ += length_;
      cursor_ = 0;
    }
  }

 private:
  uint32_t events_[kCapacity] = {};
  uint32_t start_ = 0, length_ = 0, cycleStart_ = 0, revision_ = 0;
  uint16_t count_ = 0, cursor_ = 0;
  State state_ = kEmpty;
};

}  // namespace wingie_loop
