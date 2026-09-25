#pragma once

#include <cstdint>

namespace bookorbit {

// Use uptime after an attempt this boot, so SNTP corrections cannot trigger
// repeated exchanges. Across boots use the persisted wall-clock timestamp.
inline bool progressSyncDue(uint8_t minutes, uint32_t now, uint32_t lastAttemptAt, bool lastOk, bool attemptedThisBoot,
                            uint32_t elapsedMs) {
  if (minutes == 0) return false;
  uint32_t intervalSeconds = static_cast<uint32_t>(minutes) * 60;
  if (!lastOk && intervalSeconds > 300) intervalSeconds = 300;
  if (attemptedThisBoot) return elapsedMs >= intervalSeconds * 1000;
  // Unknown time or a clock moving backwards must not postpone sync forever.
  return now == 0 || lastAttemptAt == 0 || now < lastAttemptAt || now - lastAttemptAt >= intervalSeconds;
}

}  // namespace bookorbit
