#pragma once
// DECISION-3 pending: whether TTLs are anchored to the wall clock or the
// steady (monotonic) clock — and how replay stays deterministic — is the
// human's call in CP3/CP4. This interface exposes BOTH so either resolution
// plugs in without touching call sites, and so tests can inject time.

#include <chrono>
#include <cstdint>

namespace hermit {

class Clock {
 public:
  virtual ~Clock() = default;
  // Milliseconds since the Unix epoch. Subject to NTP steps.
  virtual int64_t wall_ms() const = 0;
  // Milliseconds on a monotonic clock with an arbitrary epoch. Never steps.
  virtual int64_t steady_ms() const = 0;
};

class SystemClock final : public Clock {
 public:
  int64_t wall_ms() const override {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  }
  int64_t steady_ms() const override {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  }
};

// Test double: time moves only when told to.
class ManualClock final : public Clock {
 public:
  int64_t wall_ms() const override { return wall_; }
  int64_t steady_ms() const override { return steady_; }
  void advance(int64_t ms) {
    wall_ += ms;
    steady_ += ms;
  }
  void set_wall(int64_t ms) { wall_ = ms; }  // simulate an NTP step: wall moves, steady doesn't

 private:
  int64_t wall_ = 0;
  int64_t steady_ = 0;
};

}  // namespace hermit
