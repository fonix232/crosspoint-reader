#pragma once

#include "BookOrbitDataStore.h"

namespace bookorbit {

class ReadingTracker {
 public:
  static constexpr uint32_t MIN_PAGE_SECONDS = 5;
  static constexpr uint32_t MAX_PAGE_SECONDS = 300;
  static ReadingTracker& instance();
  void observe(const std::string& path, uint32_t spine, uint32_t page, float progress);
  void pause();
  void flush();
  static uint32_t bootId();
  static bool resolveTime(ReadingEvent& event);

 private:
  void finishPage();
  std::string path;
  uint32_t spine = 0;
  uint32_t page = 0;
  uint32_t beganMs = 0;
  ReadingEvent current;
  ReadingEvent pending[16]{};
  unsigned pendingCount = 0;
  bool active = false;
  bool idlePage = false;
  uint32_t pageSeconds = 0;
};

}  // namespace bookorbit
#define BOOKORBIT_READING bookorbit::ReadingTracker::instance()
