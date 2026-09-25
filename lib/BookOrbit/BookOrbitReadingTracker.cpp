#include "BookOrbitReadingTracker.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>

#include "BookOrbitSyncState.h"

namespace bookorbit {
namespace {
std::string anchorPath(uint32_t boot) {
  char name[64];
  snprintf(name, sizeof(name), "/.crosspoint/reading-events/clock-%08lx.bin", static_cast<unsigned long>(boot));
  return name;
}
}  // namespace

ReadingTracker& ReadingTracker::instance() {
  static ReadingTracker tracker;
  return tracker;
}

uint32_t ReadingTracker::bootId() {
  static const uint32_t id = esp_random();
  return id;
}

bool ReadingTracker::resolveTime(ReadingEvent& event) {
  if (event.stat.startTime >= 1704067200) return true;
  uint32_t anchor[2]{};
  const std::string path = anchorPath(event.boot);
  {
    HalFile file;
    if (Storage.exists(path.c_str())) {
      if (!Storage.openFileForRead("BOSTAT", path, file) || file.fileSize() != sizeof(anchor) ||
          file.read(reinterpret_cast<uint8_t*>(anchor), sizeof(anchor)) != sizeof(anchor))
        return false;
    } else {
      const uint32_t now = nowIfClockValid();
      if (event.boot != bootId() || now == 0) return false;
      anchor[0] = millis();
      anchor[1] = now;
      Storage.mkdir("/.crosspoint/reading-events", true);
      file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL);
      if (!file) return false;
      const bool written = file.write(reinterpret_cast<const uint8_t*>(anchor), sizeof(anchor)) == sizeof(anchor);
      const bool closed = file.close();
      if (!written || !closed) {
        Storage.remove(path.c_str());
        return false;
      }
    }
  }
  // Always use the first durable anchor. Retries must produce the same event
  // identity even if NTP corrects the clock between attempts.
  const int64_t elapsed = static_cast<int32_t>(anchor[0] - event.uptime) / 1000;
  const int64_t epoch = static_cast<int64_t>(anchor[1]) - elapsed;
  if (epoch < 1704067200 || epoch > UINT32_MAX) return false;
  event.stat.startTime = static_cast<uint32_t>(epoch);
  return true;
}

void ReadingTracker::observe(const std::string& nextPath, uint32_t nextSpine, uint32_t nextPage, float progress) {
  const bool samePage = path == nextPath && spine == nextSpine && page == nextPage;
  if (samePage && (active || idlePage)) return;
  finishPage();
  if (!samePage) {
    idlePage = false;
    pageSeconds = 0;
  }
  if (path != nextPath) {
    flush();
    // Keep a failed batch attached to its original book until it is durable.
    if (pendingCount != 0) return;
    path = nextPath;
  }
  spine = nextSpine;
  page = nextPage;
  beganMs = millis();
  current = ReadingEvent{};
  current.boot = bootId();
  current.uptime = beganMs;
  current.stat.startTime = nowIfClockValid();
  current.stat.page = static_cast<uint32_t>(std::clamp(progress, 0.0f, 1.0f) * 10000);
  active = true;
}

void ReadingTracker::finishPage() {
  if (!active) return;
  active = false;
  const uint32_t seconds = (static_cast<uint32_t>(millis()) - beganMs) / 1000;
  // Discard idle/outlier spans instead of turning hours on one page into five
  // minutes of reading. Redraws and menu round trips must not reset the budget.
  if (seconds > MAX_PAGE_SECONDS - pageSeconds) {
    idlePage = true;
    return;
  }
  pageSeconds += seconds;
  if (seconds < MIN_PAGE_SECONDS) return;
  current.stat.durationSeconds = seconds;
  if (pendingCount == 16) flush();
  if (pendingCount == 16) {
    LOG_ERR("BOSTAT", "reading queue unavailable");
    return;
  }
  pending[pendingCount++] = current;
  if (pendingCount == 16) flush();
}

void ReadingTracker::flush() {
  if (pendingCount == 0 || path.empty()) return;
  for (unsigned i = 0; i < pendingCount; ++i) resolveTime(pending[i]);
  ReadingQueue queue(path);
  if (queue.append(pending, pendingCount))
    pendingCount = 0;
  else
    LOG_ERR("BOSTAT", "cannot persist reading events");
}

void ReadingTracker::pause() {
  finishPage();
  flush();
}

}  // namespace bookorbit
