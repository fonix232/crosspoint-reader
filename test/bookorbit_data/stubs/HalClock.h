#pragma once
#include <ctime>
struct HalClock {
  bool available = false;
  time_t epoch = 0;
  void setTimezone(const char*) {}
  bool syncFromNTP() { return false; }
  bool isAvailable() const { return available; }
  bool utcNow(time_t& out) const {
    out = epoch;
    return available;
  }
};
inline HalClock halClock;
