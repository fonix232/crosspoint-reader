#pragma once

#include <cstdint>
#include <string>

namespace bookorbit {

// Outcome of the last network session for one account, shown by the Library
// and settings as the cached/offline indicator.
struct SyncState {
  uint32_t lastAttemptAt = 0;  // Unix seconds, 0 when the clock was invalid
  uint32_t lastSuccessAt = 0;
  bool lastOk = false;
  uint32_t progressAttemptAt = 0;  // persisted periodic exchange schedule, Unix seconds
  bool progressAttemptOk = false;
  uint32_t serverTotal = 0;
  uint16_t cachedBooks = 0;
  uint8_t lastErrorCode = 0;  // ClientError of the last failure
  // Reading insights: when the cached snapshot was fetched (0 = never) and
  // whether the server answered the insights routes at all (older BookOrbit).
  uint32_t insightsAt = 0;
  bool insightsUnsupported = false;
  bool insightsDisabled = false;  // the server denied insights access (KOReader sync disabled)
};

bool loadSyncState(const std::string& accountKey, SyncState& out);
bool saveSyncState(const std::string& accountKey, const SyncState& state);

// Unix seconds when the wall clock looks valid, else 0. Timestamps built from
// an unset clock must not be presented as real dates.
uint32_t nowIfClockValid();

}  // namespace bookorbit
