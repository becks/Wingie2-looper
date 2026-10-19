#include "Wingie2/dual_looper.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>
int main() {
  uint8_t a[256], b[300];
  wingie_loop::Loop left, right, unavailable;
  left.attach(a,256); right.attach(b,300);
  left.command(1); right.command(1);
  for(int i=0;i<1800;++i) { left.process(.4f,1,1,.9f,0); right.process(-.2f,1,1,.9f,0); }
  left.command(2);
  for(int i=0;i<900;++i) { left.process(0,1,1,.9f,0); right.process(-.2f,1,1,.9f,0); }
  assert(left.length()==200 && right.length()==300 && right.state()==2);
  for(int i=0;i<10000;++i) {
    float x=left.process(0,1.37f,1,.9f,0); assert(std::isfinite(x) && std::abs(x)<=1);
  }
  left.command(3);
  for(int i=0;i<500;++i) left.process(100,1,1,1,100);
  left.command(4); assert(left.length()==0 && left.state()==0);
  left.command(2); assert(left.state()==0);
  unavailable.command(1); assert(unavailable.process(.3f,1,1,1,0)==.3f);
  left.command(1);
  for(int i=0;i<256*9;++i) left.process(.4f,1,1,1,0);
  assert(left.length()==256 && left.state()==2);
  assert(left.takeFilled() && !left.takeFilled());
  assert(right.takeFilled());

  uint8_t compandedData[64];
  wingie_loop::Loop companded;
  companded.attach(compandedData, 64);
  companded.command(1);
  for(int i=0;i<64*9;++i) companded.process(.1f,1,1,1,0);
  float peak = 0;
  for(int i=0;i<1000;++i) peak = std::max(peak, std::abs(companded.process(0,1,1,1,0)));
  assert(peak > .07f && peak < .13f);

  uint8_t monitorData[64];
  wingie_loop::Loop monitored;
  monitored.attach(monitorData, 64);
  monitored.command(1);
  for(int i=0;i<64*9;++i) monitored.process(0, 1, 1, 1, 0);
  monitored.command(3);
  float monitoredOut = monitored.process(.4f, 1, 1, 1, 0);
  assert(monitoredOut > .19f && monitoredOut < .21f);

  // Reverse only changes playback traversal; the stored loop survives.
  for(int i=0;i<4000;++i) {
    float x = companded.processReverse(0, 1.0f, 1, 1, 0);
    assert(std::isfinite(x) && std::abs(x) <= 1);
  }

  // Chunks may live in separate heap regions; recording and playback must
  // cross chunk boundaries transparently.
  {
    const size_t chunk = wingie_loop::Loop::kChunkSize;
    std::vector<std::vector<uint8_t>> storage(3, std::vector<uint8_t>(chunk));
    uint8_t* table[3] = {storage[2].data(), storage[0].data(), storage[1].data()};
    wingie_loop::Loop chunked;
    chunked.attach(table, 3);
    chunked.command(1);
    for(size_t i=0;i<3*chunk*9;++i) chunked.process(.25f,1,1,1,0);
    assert(chunked.length()==3*chunk && chunked.state()==2);
    float lowest = 1;
    for(size_t i=0;i<3*chunk*9;++i) {
      float x = chunked.process(0,1,1,1,0);
      size_t p = chunked.position();
      if (p > 16 && p < 3*chunk-16) lowest = std::min(lowest, x);
    }
    assert(lowest > .2f);
  }

  // A float position drifts by percent-level amounts at ~150k frames; the
  // split integer/fraction position must return to the start after one cycle.
  {
    const size_t frames = 150000;
    std::vector<uint8_t> big(frames + 1000);
    wingie_loop::Loop longLoop;
    longLoop.attach(big.data(), big.size());
    longLoop.command(1);
    for(size_t i=0;i<frames*9;++i) longLoop.process(.1f,1,1,1,0);
    longLoop.command(2);
    assert(longLoop.length()==frames && longLoop.position()==0);
    for(size_t i=0;i<frames*9;++i) longLoop.process(0,1,1,1,0);
    size_t p = longLoop.position();
    assert(p <= 1 || p >= frames-1);
    for(size_t i=0;i<frames*9;++i) longLoop.processReverse(0,1,1,1,0);
    p = longLoop.position();
    assert(p <= 1 || p >= frames-1);
  }
}
