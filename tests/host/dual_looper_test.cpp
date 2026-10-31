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

  // Playback replaces the live input unless live input is on (-6 dB, like overdub).
  monitored.command(2);
  for(int i=0;i<64*9;++i) monitored.process(0, 1, 1, 1, 0);
  float replaced = monitored.process(.4f, 1, 1, 1, 0);
  monitored.setLiveInput(true);
  float mixed = monitored.process(.4f, 1, 1, 1, 0);
  assert(std::abs(mixed - replaced - .2f) < .02f);
  monitored.setLiveInput(false);
  assert(std::abs(monitored.process(.4f, 1, 1, 1, 0) - replaced) < .02f);

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
    // One full forward cycle above crossed the start exactly once.
    assert(chunked.takeWrapped() && !chunked.takeWrapped());
  }

  // A float position drifts by percent-level amounts past ~100k frames; the
  // split integer/fraction position must return to the start after one cycle.
  {
    const size_t capacity = wingie_loop::Loop::kMaxChunks * wingie_loop::Loop::kChunkSize;
    const size_t frames = capacity - 1000;
    std::vector<uint8_t> big(capacity);
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

  // Pitch shift (stretch mode): grains change the pitch by the ratio while the
  // play position, and so the loop length, still advance at 1x.
  {
    const size_t frames = 4900;  // 1 s loop of a ~408 Hz sine (12 frames per cycle)
    std::vector<uint8_t> storage(frames);
    wingie_loop::Loop shifted;
    shifted.attach(storage.data(), storage.size());
    shifted.command(1);
    for (size_t i = 0; i < frames * 9; ++i)
      shifted.process(0.5f * sinf(6.2831853f * (i / 9.0f) / 12.0f), 1, 1, 1, 0);
    shifted.command(2);
    assert(shifted.length() == frames);
    const float step = 1.0f / wingie_loop::Loop::rateDivision();
    // -2 octaves is left out: two-grain shifting smears it far more.
    for (float pitch : {0.5f, 2.0f / 3.0f, 0.75f, 4.0f / 3.0f, 1.5f, 2.0f}) {
      for (bool reverse : {false, true}) {
        const int n = 64, blocks = (int)(frames * 9 / n);  // one loop cycle
        size_t start = shifted.position();
        float in[64] = {}, out[64];
        std::vector<float> played;
        float peak = 0;
        for (int blk = 0; blk < blocks; ++blk) {
          if (reverse) shifted.processReverse(in, out, n, step, 1, 1, nullptr, 0, pitch);
          else shifted.process(in, out, n, step, 1, 1, nullptr, 0, pitch);
          for (int k = 0; k < n; ++k) {
            assert(std::isfinite(out[k]) && std::abs(out[k]) <= 1);
            played.push_back(out[k]);
            peak = std::max(peak, std::abs(out[k]));
          }
        }
        // Back near the start after one cycle at 1x, whatever the pitch.
        size_t p = shifted.position(), d = p > start ? p - start : start - p;
        assert(d <= 2 || d >= frames - 2);
        // The recorded sine has a 108-sample period at 1x. Grain seams add
        // sidebands, so the strongest frequency lands within 8% of the ratio;
        // the level stays close to the recorded one.
        double best = 0, bestRatio = 0;
        for (double ratio = 0.2; ratio <= 2.5; ratio += 0.005) {
          double w = 2 * M_PI * ratio / 108.0, c = 2 * cos(w), s1 = 0, s2 = 0;
          for (float v : played) { double s0 = v + c * s1 - s2; s2 = s1; s1 = s0; }
          double power = s1 * s1 + s2 * s2 - c * s1 * s2;
          if (power > best) { best = power; bestRatio = ratio; }
        }
        assert(std::abs(bestRatio - pitch) < 0.08 * pitch);
        assert(peak > 0.4f && peak < 0.6f);
      }
    }
    // Stretch: the play position moves at the tempo while grains keep the
    // recorded pitch (grain ratio 1 / tempo).
    for (float tempo : {0.5f, 2.0f / 3.0f, 1.5f, 2.0f}) {
      const int n = 64, blocks = (int)(frames * 9 / n);
      size_t start = shifted.position();
      float in[64] = {}, out[64];
      std::vector<float> played;
      for (int blk = 0; blk < blocks; ++blk) {
        shifted.process(in, out, n, tempo * step, 1, 1, nullptr, 0, 1.0f / tempo);
        played.insert(played.end(), out, out + n);
      }
      // One loop time at 1x covers `tempo` loops.
      double covered = std::fmod(tempo, 1.0) * frames;
      size_t p = shifted.position();
      double moved = p >= start ? p - start : p + frames - start;
      assert(std::abs(moved - covered) <= 2 || std::abs(moved - covered) >= frames - 2);
      double best = 0, bestRatio = 0;
      for (double ratio = 0.2; ratio <= 2.5; ratio += 0.005) {
        double w = 2 * M_PI * ratio / 108.0, c = 2 * cos(w), s1 = 0, s2 = 0;
        for (float v : played) { double s0 = v + c * s1 - s2; s2 = s1; s1 = s0; }
        double power = s1 * s1 + s2 * s2 - c * s1 * s2;
        if (power > best) { best = power; bestRatio = ratio; }
      }
      assert(std::abs(bestRatio - 1.0) < 0.08);
    }

    // Pitch 1 is the plain path.
    std::vector<uint8_t> plainStorage(frames), unityStorage(frames);
    wingie_loop::Loop plain, unity;
    plain.attach(plainStorage.data(), frames);
    unity.attach(unityStorage.data(), frames);
    plain.command(1); unity.command(1);
    for (size_t i = 0; i < 900; ++i) { float v = 0.3f * sinf(i * 0.05f); plain.process(v, 1, 1, 1, 0); unity.process(v, 1, 1, 1, 0); }
    plain.command(2); unity.command(2);
    float x[64] = {}, y[64], z[64];
    for (int blk = 0; blk < 4; ++blk) {
      plain.process(x, y, 64, step, 1, 1, nullptr, 0);
      unity.process(x, z, 64, step, 1, 1, nullptr, 0, 1.0f);
      for (int k = 0; k < 64; ++k) assert(y[k] == z[k]);
    }
  }

  // Waveform peaks: per-bucket mu-law magnitude, empty loops read as silence.
  {
    std::vector<uint8_t> buf(wingie_loop::Loop::kChunkSize * 2);
    wingie_loop::Loop w;
    w.attach(buf.data(), buf.size());
    uint8_t out[4] = {9, 9, 9, 9};
    w.peaks(out, 4);
    assert(out[0] == 0 && out[3] == 0);
    w.command(1);
    for (int i = 0; i < 400 * 9; ++i) w.process(i < 200 * 9 ? 0.0f : 0.8f, 1, 1, 1, 0);
    w.command(2);
    assert(w.length() == 400 && w.capacity() == buf.size());
    w.peaks(out, 4);
    assert(out[0] == 0 && out[1] == 0 && out[2] > 100 && out[3] > 100);
  }

  // After the resonator the input (its output) passes at unity under the loop.
  {
    uint8_t data[64];
    wingie_loop::Loop post;
    post.attach(data, 64);
    post.command(1);
    for (int i = 0; i < 64 * 9; ++i) post.process(0, 1, 1, 1, 0);
    assert(post.state() == 2);
    post.setPost(true);
    assert(std::abs(post.process(.4f, 1, 1, 1, 0) - .4f) < .001f);
    post.command(3);
    assert(std::abs(post.process(.4f, 1, 1, 1, 0) - .4f) < .001f);
  }

  // Gate mode: silent until the gate opens; a trigger restarts from the loop
  // start after fading out, and releasing the gate fades it out again.
  {
    std::vector<uint8_t> buf(400);
    wingie_loop::Loop g;
    g.attach(buf.data(), buf.size());
    g.command(1);
    for (int i = 0; i < 400 * 9; ++i) g.process(i < 200 * 9 ? 0.5f : -0.5f, 1, 1, 1, 0);
    assert(g.state() == 2 && g.length() == 400);
    for (int i = 0; i < 100 * 9; ++i) g.process(0, 1, 1, 1, 0);  // play into the first half
    g.setGate(true, false);
    float last = 1;
    for (int i = 0; i < 400; ++i) last = g.process(0, 1, 1, 1, 0);
    assert(std::abs(last) < .001f);
    // Advance into the second (negative) half, then trigger: playback restarts in the first.
    for (int i = 0; i < 150 * 9; ++i) g.process(0, 1, 1, 1, 0);
    assert(g.position() > 200);
    g.setGate(true, true);
    g.retrigger();
    for (int i = 0; i < 20 * 9; ++i) last = g.process(0, 1, 1, 1, 0);
    assert(g.position() < 40 && last > .4f);
    // Retriggering while open fades out first, with no jump larger than the fade allows.
    for (int i = 0; i < 250 * 9; ++i) g.process(0, 1, 1, 1, 0);
    assert(g.position() > 250);
    g.retrigger();
    float previous = g.process(0, 1, 1, 1, 0), biggest = 0;
    for (int i = 0; i < 400; ++i) {
      float x = g.process(0, 1, 1, 1, 0);
      biggest = std::max(biggest, std::abs(x - previous));
      previous = x;
    }
    assert(biggest < .05f && g.position() < 60 && previous > .4f);
    g.setGate(true, false);
    for (int i = 0; i < 400; ++i) last = g.process(0, 1, 1, 1, 0);
    assert(std::abs(last) < .001f);
    // Overdub ignores the gate; leaving gate mode makes playback audible again.
    g.command(3);
    for (int i = 0; i < 400; ++i) last = g.process(0, 1, 1, 0, 0);
    g.command(2);
    g.setGate(false, false);
    for (int i = 0; i < 400; ++i) last = g.process(0, 1, 1, 1, 0);
    assert(g.state() == 2);
  }

  // Playback window: start/end as shares of the recording.
  {
    std::vector<uint8_t> buf(1000);
    wingie_loop::Loop w;
    w.attach(buf.data(), buf.size());
    w.command(1);
    for (int i = 0; i < 1000 * 9; ++i) w.process(i < 400 * 9 ? 0.5f : i < 800 * 9 ? -0.5f : 0.25f, 1, 1, 1, 0);
    assert(w.state() == 2 && w.length() == 1000);
    for (int i = 0; i < 100 * 9; ++i) w.process(0, 1, 1, 1, 0);
    assert(w.position() < 400);
    // Window away from the play position: fade out, enter it at its start, no click.
    w.setWindow(0.4f, 0.8f);
    float previous = w.process(0, 1, 1, 1, 0), biggest = 0;
    for (int i = 0; i < 400; ++i) {
      float x = w.process(0, 1, 1, 1, 0);
      biggest = std::max(biggest, std::abs(x - previous));
      previous = x;
    }
    assert(biggest < .05f && previous < -.4f);
    assert(w.position() >= 400 && w.position() < 460);
    // Playback stays inside it, wrapping at its end.
    float highest = -1;
    for (int i = 0; i < 2000 * 9; ++i) {
      highest = std::max(highest, w.process(0, 1, 1, 1, 0));
      assert(w.position() >= 400 && w.position() < 800);
    }
    assert(highest < .01f);
    assert(w.takeWrapped());
    // Moving an edge while the play position stays inside does not move it.
    size_t before = w.position();
    w.setWindow(0.3f, 0.8f);
    w.process(0, 1, 1, 1, 0);
    assert(w.position() >= before && w.position() <= before + 1);
    w.setWindow(0.4f, 0.8f);
    // Reverse wraps inside the window too.
    for (int i = 0; i < 2000 * 9; ++i) {
      w.processReverse(0, 1, 1, 1, 0);
      assert(w.position() >= 400 && w.position() < 800);
    }
    // Pitch shift (grains) reads inside the window.
    float in = 0, out = 0;
    for (int i = 0; i < 2000 * 9; ++i) {
      w.process(&in, &out, 1, 1.0f / 9, 1, 1, nullptr, 0, 1.5f);
      assert(std::isfinite(out) && out < .01f);
      assert(w.position() >= 400 && w.position() < 800);
    }
    // Overdub writes only inside the window.
    w.command(3);
    for (int i = 0; i < 1000 * 9; ++i) w.process(0.2f, 1, 1, 1, 0);
    w.command(2);
    w.setWindow(0, 0.3f);
    for (int i = 0; i < 500; ++i) w.process(0, 1, 1, 1, 0);
    for (int i = 0; i < 200 * 9; ++i) {
      float x = w.process(0, 1, 1, 1, 0);
      size_t p = w.position();
      if (p > 10 && p < 290) assert(std::abs(x - 0.5f) < .03f);
    }
    // Start past the end: the window is kept at its minimum length.
    w.setWindow(0.9f, 0.5f);
    for (int i = 0; i < 500; ++i) w.process(0, 1, 1, 1, 0);
    for (int i = 0; i < 1000 * 9; ++i) {
      w.process(0, 1, 1, 1, 0);
      assert(w.position() >= 900 && w.position() < 964);
    }
    // Clear forgets the window frames; a new recording plays whole.
    w.command(4);
    w.setWindow(0, 1);
    w.command(1);
    for (int i = 0; i < 300 * 9; ++i) w.process(0.5f, 1, 1, 1, 0);
    w.command(2);
    for (int i = 0; i < 600 * 9; ++i) {
      w.process(0, 1, 1, 1, 0);
      assert(w.position() < 300);
    }
  }
}
