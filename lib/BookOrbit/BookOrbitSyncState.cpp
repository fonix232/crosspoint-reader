#include "BookOrbitSyncState.h"

#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <PersistableStore.h>

#include <ctime>

#include "BookOrbitPaths.h"

namespace bookorbit {

namespace {
// Anything earlier than this is the epoch default of an unset RTC.
constexpr uint32_t MIN_PLAUSIBLE_TIME = 1704067200;  // 2024-01-01
}  // namespace

bool loadSyncState(const std::string& accountKey, SyncState& out) {
  out = SyncState{};
  JsonDocument doc;
  const std::string path = syncStatePath(accountKey);
  if (!Storage.exists(path.c_str()) || !PersistableStoreBase::readDocFromFile(path.c_str(), doc)) return false;
  out.lastAttemptAt = doc["lastAttemptAt"] | 0u;
  out.lastSuccessAt = doc["lastSuccessAt"] | 0u;
  out.lastOk = doc["lastOk"] | false;
  out.progressAttemptAt = doc["progressAttemptAt"] | 0u;
  out.progressAttemptOk = doc["progressAttemptOk"] | false;
  out.serverTotal = doc["serverTotal"] | 0u;
  out.cachedBooks = doc["cachedBooks"] | 0;
  out.lastErrorCode = doc["lastError"] | 0;
  out.insightsAt = doc["insightsAt"] | 0u;
  out.insightsUnsupported = doc["insightsUnsupported"] | false;
  out.insightsDisabled = doc["insightsDisabled"] | false;
  return true;
}

bool saveSyncState(const std::string& accountKey, const SyncState& state) {
  Storage.mkdir(accountDir(accountKey).c_str());
  JsonDocument doc;
  doc["lastAttemptAt"] = state.lastAttemptAt;
  doc["lastSuccessAt"] = state.lastSuccessAt;
  doc["lastOk"] = state.lastOk;
  doc["progressAttemptAt"] = state.progressAttemptAt;
  doc["progressAttemptOk"] = state.progressAttemptOk;
  doc["serverTotal"] = state.serverTotal;
  doc["cachedBooks"] = state.cachedBooks;
  doc["lastError"] = state.lastErrorCode;
  doc["insightsAt"] = state.insightsAt;
  doc["insightsUnsupported"] = state.insightsUnsupported;
  doc["insightsDisabled"] = state.insightsDisabled;
  return PersistableStoreBase::writeDocToFile(syncStatePath(accountKey).c_str(), doc);
}

uint32_t nowIfClockValid() {
  time_t now = time(nullptr);
  if (now < static_cast<time_t>(MIN_PLAUSIBLE_TIME)) {
    if (!halClock.isAvailable() || !halClock.utcNow(now) || now < static_cast<time_t>(MIN_PLAUSIBLE_TIME)) return 0;
  }
  return static_cast<uint32_t>(now);
}

}  // namespace bookorbit
