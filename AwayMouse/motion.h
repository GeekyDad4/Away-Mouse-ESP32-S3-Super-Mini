// motion.h - human-like, net-zero mouse motion engine.
//
// Platform independent: the firmware and the desktop simulator both include
// this file. All I/O goes through the MouseSink interface.
//
// A burst is a few curved strokes that wander away from the starting point,
// a pause, and then the exact same reports played back negated in the same
// order. Every outbound report has an equal-and-opposite report with the
// same size and timing, so OS pointer acceleration scales both identically
// and they cancel. The cursor ends each burst where it started.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

struct MotionConfig {
  int   movesMin;           // strokes per burst (outbound)
  int   movesMax;
  float wanderPx;           // max distance from the burst's starting point
  float speed;              // 1.0 = normal, 2.0 = twice as fast
  float overshootChance;    // 0..1, chance a stroke overshoots and corrects
  float tremorPx;           // hand-tremor amplitude
  float scrollChance;       // 0..1, chance a burst includes a scroll
  int   scrollNotchesMin;
  int   scrollNotchesMax;
};

// What the engine needs from the outside world.
class MouseSink {
public:
  virtual ~MouseSink() {}
  // Send one report. Return false if it could not be delivered.
  virtual bool send(int8_t dx, int8_t dy, int8_t wheel) = 0;
  // Still connected and allowed to move?
  virtual bool alive() = 0;
  // Wait until the next report slot (fixed cadence).
  virtual void waitTick() = 0;
  // Restart the cadence clock (after a pause).
  virtual void resetTick() = 0;
  // Plain sleep.
  virtual void sleepMs(uint32_t ms) = 0;
  // Report interval of the current link in ms.
  virtual uint32_t tickMs() = 0;
  // 32 random bits.
  virtual uint32_t random32() = 0;
};

class MotionEngine {
public:
  MotionEngine(const MotionConfig &cfg, MouseSink &sink) : cfg_(cfg), sink_(sink) {}

  // Runs one complete burst. Returns false if the link dropped mid-burst
  // (in that case the cursor may be left slightly off its starting point).
  bool runBurst() {
    events_.clear();
    events_.reserve(2048);
    netX_ = 0;
    netY_ = 0;

    int moves = irand(cfg_.movesMin, cfg_.movesMax);
    int scrollAfter = (frand() < cfg_.scrollChance) ? irand(0, moves - 1) : -1;

    // ---- outbound ----
    for (int m = 0; m < moves; m++) {
      // never let the recording overflow: it has to hold the whole way out
      if (events_.size() > kMaxEvents - 1400) break;
      if (m > 0 && !pause(betweenStrokesMs())) return false;
      int tx, ty;
      pickTarget(tx, ty);
      if (!strokeTo(tx, ty, true)) return false;
      if (m == scrollAfter && !scrollOutAndBack()) return false;
    }

    // ---- linger, then come home along the mirrored path ----
    if (!idle(irand(600, 2500))) return false;
    sink_.resetTick();
    for (const Ev &e : events_) {
      if (e.kind == Ev::PAUSE) {
        if (!idle(e.ms)) return false;
        sink_.resetTick();
      } else if (e.kind == Ev::SKIP) {
        sink_.waitTick();  // a quiet report slot, same as on the way out
      } else {
        sink_.waitTick();
        if (!sendReliable(-e.dx, -e.dy, 0)) return false;
      }
    }
    return true;
  }

  // Exposed for the simulator.
  int netX() const { return netX_; }
  int netY() const { return netY_; }

private:
  struct Ev {
    enum Kind : uint8_t { REPORT, PAUSE, SKIP } kind;
    int8_t dx, dy;
    uint16_t ms;
  };

  const MotionConfig &cfg_;
  MouseSink &sink_;
  std::vector<Ev> events_;
  int netX_ = 0, netY_ = 0;   // where we are relative to the burst start
  static constexpr size_t kMaxEvents = 6000;

  static int clamp127(int v) { return v > 127 ? 127 : (v < -127 ? -127 : v); }

  // ---------- random helpers ----------
  float frand() { return (sink_.random32() >> 8) * (1.0f / 16777216.0f); }
  float frange(float a, float b) { return a + (b - a) * frand(); }
  int irand(int a, int b) { return b <= a ? a : a + (int)(sink_.random32() % (uint32_t)(b - a + 1)); }
  float gauss() {  // cheap approx normal(0,1)
    return (frand() + frand() + frand() + frand() - 2.0f) * 1.732f;
  }

  int betweenStrokesMs() {
    // mostly short hesitations, sometimes a longer hover
    return frand() < 0.2f ? irand(900, 2600) : irand(120, 650);
  }

  void pickTarget(int &tx, int &ty) {
    const float W = cfg_.wanderPx;
    const float minDist = std::fmax(25.0f, 0.25f * W);
    float bx = 0, by = 0;
    for (int i = 0; i < 12; i++) {
      float r = W * std::sqrt(frand());
      float a = frand() * 6.2831853f;
      float x = r * std::cos(a), y = r * std::sin(a);
      bx = x; by = y;
      if (std::hypot(x - netX_, y - netY_) >= minDist) break;
    }
    tx = (int)std::lround(bx);
    ty = (int)std::lround(by);
  }

  // ---------- recording helpers ----------
  void recordReport(int dx, int dy) {
    if (events_.size() < kMaxEvents) events_.push_back(Ev{Ev::REPORT, (int8_t)dx, (int8_t)dy, 0});
  }
  void recordSkip() {
    if (events_.size() < kMaxEvents) events_.push_back(Ev{Ev::SKIP, 0, 0, 0});
  }
  void recordPause(int ms) {
    if (events_.size() < kMaxEvents) events_.push_back(Ev{Ev::PAUSE, 0, 0, (uint16_t)ms});
  }

  // Send with a few retries (Bluetooth can be briefly out of buffers).
  bool sendReliable(int dx, int dy, int wheel) {
    for (int attempt = 0; attempt < 20; attempt++) {
      if (!sink_.alive()) return false;
      if (sink_.send((int8_t)dx, (int8_t)dy, (int8_t)wheel)) return true;
      sink_.sleepMs(4);
    }
    return false;
  }

  // Sleep in small slices so a disconnect is noticed quickly.
  bool idle(int ms) {
    while (ms > 0) {
      if (!sink_.alive()) return false;
      int step = ms > 50 ? 50 : ms;
      sink_.sleepMs(step);
      ms -= step;
    }
    return sink_.alive();
  }

  // Pause that is part of the outbound path (replayed on the way back).
  bool pause(int ms) {
    if (ms > 60000) ms = 60000;
    if (events_.size() >= kMaxEvents) return false;
    recordPause(ms);
    bool ok = idle(ms);
    sink_.resetTick();
    return ok;
  }

  // Curved, minimum-jerk stroke from the current position to (tx,ty).
  // With `allowOvershoot`, sometimes lands past the target and corrects.
  bool strokeTo(int tx, int ty, bool allowOvershoot) {
    float dx = (float)(tx - netX_), dy = (float)(ty - netY_);
    float dist = std::hypot(dx, dy);
    if (dist < 1.0f) return true;

    if (allowOvershoot && dist > 40.0f && frand() < cfg_.overshootChance) {
      float os = frange(0.04f, 0.12f);
      float ux = dx / dist, uy = dy / dist;
      float side = frange(-0.04f, 0.04f) * dist;
      int ox = (int)std::lround(tx + ux * os * dist - uy * side);
      int oy = (int)std::lround(ty + uy * os * dist + ux * side);
      if (!curve(ox, oy, 1.0f)) return false;
      if (!pause(irand(40, 160))) return false;
      return curve(tx, ty, 1.6f);  // quick little correction
    }
    return curve(tx, ty, 1.0f);
  }

  bool curve(int tx, int ty, float speedBoost) {
    const float sx = (float)netX_, sy = (float)netY_;
    const float dx = tx - sx, dy = ty - sy;
    const float dist = std::hypot(dx, dy);
    if (dist < 1.0f) return true;

    // Fitts-ish duration: longer moves take longer, but sub-linearly.
    float durMs = (170.0f + 115.0f * std::log2(1.0f + dist / 18.0f)) * frange(0.85f, 1.2f);
    durMs /= (cfg_.speed > 0.05f ? cfg_.speed : 0.05f) * speedBoost;
    const float tick = (float)sink_.tickMs();
    int n = (int)std::lround(durMs / tick);
    // keep each report well inside the int8 range
    int minN = (int)std::ceil(dist / 60.0f) + 2;
    if (n < minN) n = minN;
    if (n < 3) n = 3;
    if (n > 600) n = 600;

    // Cubic Bezier with control points bowed off to one side (slightly
    // asymmetric, like a wrist pivot).
    const float px = -dy / dist, py = dx / dist;
    const float bend = dist * frange(-0.22f, 0.22f);
    const float c1x = sx + dx * frange(0.2f, 0.4f) + px * bend * frange(0.7f, 1.2f);
    const float c1y = sy + dy * frange(0.2f, 0.4f) + py * bend * frange(0.7f, 1.2f);
    const float c2x = sx + dx * frange(0.6f, 0.8f) + px * bend * frange(0.4f, 1.0f);
    const float c2y = sy + dy * frange(0.6f, 0.8f) + py * bend * frange(0.4f, 1.0f);
    const float skew = frange(0.8f, 0.95f);  // peak velocity a bit before halfway

    float trX = 0, trY = 0;  // low-passed tremor
    int sentX = netX_, sentY = netY_;
    sink_.resetTick();
    for (int i = 1; i <= n; i++) {
      float t = (float)i / n;
      float u = std::pow(t, skew);
      float s = u * u * u * (10.0f + u * (-15.0f + 6.0f * u));  // min-jerk
      float a = 1 - s;
      float x = a * a * a * sx + 3 * a * a * s * c1x + 3 * a * s * s * c2x + s * s * s * tx;
      float y = a * a * a * sy + 3 * a * a * s * c1y + 3 * a * s * s * c2y + s * s * s * ty;

      trX = 0.75f * trX + 0.25f * gauss() * cfg_.tremorPx * 2.0f;
      trY = 0.75f * trY + 0.25f * gauss() * cfg_.tremorPx * 2.0f;
      float env = std::sin(3.1415926f * t);  // no tremor at the endpoints
      if (i < n) { x += trX * env; y += trY * env; }

      int ix = (i == n) ? tx : (int)std::lround(x);
      int iy = (i == n) ? ty : (int)std::lround(y);
      int ddx = ix - sentX, ddy = iy - sentY;
      ddx = clamp127(ddx);
      ddy = clamp127(ddy);

      if (ddx == 0 && ddy == 0) {
        // a quiet slot still takes time; replay it as one
        recordSkip();
        sink_.waitTick();
        continue;
      }
      sink_.waitTick();
      if (!sendReliable(ddx, ddy, 0)) return false;
      recordReport(ddx, ddy);
      sentX += ddx; sentY += ddy;
      netX_ = sentX; netY_ = sentY;
    }
    // clamp could (in theory) leave a remainder: finish it off
    while (sentX != tx || sentY != ty) {
      int ddx = tx - sentX, ddy = ty - sentY;
      ddx = clamp127(ddx);
      ddy = clamp127(ddy);
      sink_.waitTick();
      if (!sendReliable(ddx, ddy, 0)) return false;
      recordReport(ddx, ddy);
      sentX += ddx; sentY += ddy;
      netX_ = sentX; netY_ = sentY;
    }
    return true;
  }

  // A few notches down, "read" for a bit, then the same number back up.
  bool scrollOutAndBack() {
    int notches = irand(cfg_.scrollNotchesMin, cfg_.scrollNotchesMax);
    if (notches <= 0) return true;
    if (!idle(irand(250, 700))) return false;
    for (int i = 0; i < notches; i++) {
      if (!sendReliable(0, 0, -1)) return false;
      if (!idle(irand(70, 220))) return false;
    }
    if (!idle(irand(1500, 5000))) return false;
    for (int i = 0; i < notches; i++) {
      if (!sendReliable(0, 0, 1)) return false;
      if (!idle(irand(70, 220))) return false;
    }
    sink_.resetTick();
    return true;
  }
};
