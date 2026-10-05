#include "Wingie2/dual_looper.h"
#include <algorithm>
#include <cassert>
#include <cmath>
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
}
