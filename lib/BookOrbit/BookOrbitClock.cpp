#include "BookOrbitClock.h"

#include <Arduino.h>
#include <HalClock.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace bookorbit {

namespace {
constexpr char TAG[] = "BOCLK";
constexpr int SNTP_WAIT_MS = 6000;
constexpr int SNTP_POLL_MS = 100;
}  // namespace

bool ensureSystemClock() {
  if (time(nullptr) >= 1704067200) return true;

  time_t rtc = 0;
  if (halClock.isAvailable() && halClock.utcNow(rtc) && rtc >= 1704067200) {
    timeval tv{};
    tv.tv_sec = rtc;
    settimeofday(&tv, nullptr);
    LOG_INF(TAG, "system clock seeded from RTC");
    return time(nullptr) >= 1704067200;
  }

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR(TAG, "no RTC time and no Wi-Fi; clock stays unset");
    return false;
  }
  // Persist network time in the RTC for offline reading after the next boot.
  if (halClock.isAvailable()) return halClock.syncFromNTP() && time(nullptr) >= 1704067200;

  // configTzTime overwrites the process TZ for the exchange; restore the
  // display rule afterwards exactly as HalClock::syncFromNTP does.
  const char* tzBefore = getenv("TZ");
  char savedTz[64] = {0};
  if (tzBefore) snprintf(savedTz, sizeof(savedTz), "%s", tzBefore);
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");
  bool synced = false;
  for (int waited = 0; waited < SNTP_WAIT_MS; waited += SNTP_POLL_MS) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      synced = true;
      break;
    }
    delay(SNTP_POLL_MS);
  }
  halClock.setTimezone(savedTz);
  if (!synced) {
    LOG_ERR(TAG, "SNTP sync timed out; clock stays unset");
    return time(nullptr) >= 1704067200;
  }
  LOG_INF(TAG, "system clock set from SNTP");
  return time(nullptr) >= 1704067200;
}

}  // namespace bookorbit
