#include "Wingie2/note_looper.h"
#include <cassert>
#include <vector>
using wingie_loop::NoteLoop;

static std::vector<int> due(NoteLoop& loop, uint32_t now) {
  std::vector<int> keys;
  uint8_t key;
  while (loop.next(now, key)) keys.push_back(key);
  return keys;
}

int main() {
  NoteLoop loop;
  assert(loop.state() == NoteLoop::kEmpty && due(loop, 0).empty());

  // Empty recordings return to the empty state.
  loop.startRecording(1000);
  loop.finishRecording(2000);
  assert(loop.state() == NoteLoop::kEmpty);

  // Record C at 100 ms and E at 600 ms into a 1000 ms loop.
  loop.startRecording(5000);
  assert(loop.recording());
  assert(!loop.record(5100, 0));
  assert(!loop.record(5600, 4));
  loop.finishRecording(6000);
  assert(loop.state() == NoteLoop::kPlaying && loop.lengthMs() == 1000 && loop.count() == 2);

  assert(due(loop, 6050).empty());
  assert((due(loop, 6100) == std::vector<int>{0}));
  assert(due(loop, 6599).empty());
  assert((due(loop, 6700) == std::vector<int>{4}));
  // Second cycle wraps and replays from the start.
  assert(due(loop, 7050).empty());
  assert((due(loop, 7150) == std::vector<int>{0}));
  // A late poll still yields every due key in order across the wrap.
  assert((due(loop, 8650) == std::vector<int>{4, 0, 4}));

  // Stop keeps the sequence; play restarts it from the loop start.
  loop.stop();
  assert(loop.state() == NoteLoop::kStopped && due(loop, 9100).empty());
  loop.play(20000);
  assert((due(loop, 20100) == std::vector<int>{0}));

  loop.clear();
  assert(loop.state() == NoteLoop::kEmpty && loop.count() == 0);
  loop.play(0);
  assert(loop.state() == NoteLoop::kEmpty);

  // A key in the closing millisecond stays inside the loop.
  loop.startRecording(0);
  loop.record(500, 7);
  loop.finishRecording(500);
  assert(loop.lengthMs() == 501);
  assert((due(loop, 1000) == std::vector<int>{7}));

  // A full buffer closes the loop and starts playback.
  loop.startRecording(0);
  bool filled = false;
  for (uint16_t i = 0; i < NoteLoop::kCapacity; ++i) filled = loop.record(i * 10, i % 12);
  assert(filled && loop.state() == NoteLoop::kPlaying && loop.count() == NoteLoop::kCapacity);
  assert(loop.lengthMs() == (NoteLoop::kCapacity - 1) * 10 + 1);
  assert(!loop.record(9999, 3));

  // millis() overflow during a loop keeps timing.
  loop.startRecording(0xfffffff0u);
  loop.record(0xfffffffau, 2);
  loop.finishRecording(20);
  assert(loop.lengthMs() == 36);
  assert((due(loop, 30) == std::vector<int>{2}));

  // Monitor accessors: events, cycle position and a revision that tracks edits.
  {
    NoteLoop m;
    uint32_t r = m.revision();
    m.startRecording(1000);
    assert(m.revision() != r && m.positionMs(1250) == 250);
    r = m.revision();
    m.record(1100, 5);
    assert(m.revision() != r && m.eventTime(0) == 100 && m.eventKey(0) == 5);
    assert(m.eventTime(1) == 0 && m.eventKey(1) == 0);
    m.finishRecording(1400);
    r = m.revision();
    assert(m.positionMs(1400) == 0 && m.positionMs(1850) == 50);
    m.stop();
    assert(m.positionMs(1750) == 0 && m.revision() == r);
    m.clear();
    assert(m.revision() != r);
  }
}
