#include "BookOrbitInsightsCache.h"

#include <HalStorage.h>
#include <Memory.h>

#include <array>
#include <cstdio>
#include <cstring>

#include "BookOrbitPaths.h"
#include "BookOrbitStorage.h"

namespace bookorbit {

namespace {
constexpr char TAG[] = "BOINS";
}  // namespace

std::string insightsDir(const std::string& accountKey) { return accountDir(accountKey) + "/insights"; }
std::string insightsSummaryPath(const std::string& accountKey) { return insightsDir(accountKey) + "/summary.json"; }
std::string insightsHabitsPath(const std::string& accountKey) { return insightsDir(accountKey) + "/habits.json"; }
std::string insightsCalendarPath(const std::string& accountKey, const uint16_t year, const uint8_t month) {
  char name[24];
  snprintf(name, sizeof(name), "/cal-%04u-%02u.json", static_cast<unsigned>(year), static_cast<unsigned>(month));
  return insightsDir(accountKey) + name;
}

namespace {
// Parser state exceeds the embedded stack budget; allocate once per cache load.
template <typename Parser, typename Record>
bool loadThrough(const std::string& path, Record& out) {
  auto parser = makeUniqueNoThrow<Parser>(out);
  if (!parser) return false;
  HalFile file;
  if (!storage::recover(path) || !Storage.openFileForRead(TAG, path, file)) return false;
  if (file.fileSize() > INSIGHTS_MAX_JSON) return false;
  char buf[192];
  int n;
  while ((n = file.read(buf, sizeof(buf))) > 0) parser->feed(buf, static_cast<size_t>(n));
  return n == 0 && parser->finish();
}
}  // namespace

bool loadInsightsSummary(const std::string& accountKey, InsightsSummary& out) {
  return loadThrough<InsightsSummaryParser>(insightsSummaryPath(accountKey), out);
}

bool loadInsightsHabits(const std::string& accountKey, InsightsHabits& out) {
  return loadThrough<InsightsHabitsParser>(insightsHabitsPath(accountKey), out);
}

bool loadInsightsCalendar(const std::string& accountKey, const uint16_t year, const uint8_t month,
                          InsightsCalendar& out) {
  return loadThrough<InsightsCalendarParser>(insightsCalendarPath(accountKey, year, month), out);
}

bool saveInsightsBody(const std::string& path, const std::string& body) {
  const size_t slash = path.rfind('/');
  if (slash != std::string::npos) Storage.mkdir(path.substr(0, slash).c_str());
  const std::string tmp = path + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite(TAG, tmp, file)) return false;
  const size_t written = file.write(body.data(), body.size());
  const bool closed = file.close();
  if (written != body.size() || !closed) {
    Storage.remove(tmp.c_str());
    return false;
  }
  if (!storage::replace(tmp, path)) {
    Storage.remove(tmp.c_str());
    return false;
  }
  return true;
}

void pruneInsightsCalendars(const std::string& accountKey, const uint8_t keepMonths) {
  const std::string dir = insightsDir(accountKey);
  HalFile handle = Storage.open(dir.c_str());
  if (!handle || !handle.isDirectory()) return;
  // Names sort chronologically ("cal-YYYY-MM.json"); a bounded table of the
  // newest is enough to know what to drop.
  using Name = std::array<char, 24>;
  auto keep = makeUniqueNoThrow<Name[]>(12);
  if (!keep) return;
  uint8_t kept = 0;
  const uint8_t cap = keepMonths > 12 ? 12 : keepMonths;
  char name[40];
  for (HalFile entry = handle.openNextFile(); entry; entry = handle.openNextFile()) {
    entry.getName(name, sizeof(name));
    entry.close();
    if (strncmp(name, "cal-", 4) != 0 || strlen(name) >= sizeof(keep[0])) continue;
    // Insert, keeping the table sorted descending.
    uint8_t pos = 0;
    while (pos < kept && strcmp(keep[pos].data(), name) > 0) pos++;
    if (pos >= cap) continue;
    for (uint8_t i = kept < cap ? kept : cap - 1; i > pos; i--) strcpy(keep[i].data(), keep[i - 1].data());
    strcpy(keep[pos].data(), name);
    if (kept < cap) kept++;
  }
  handle.rewindDirectory();
  for (HalFile entry = handle.openNextFile(); entry; entry = handle.openNextFile()) {
    entry.getName(name, sizeof(name));
    entry.close();
    if (strncmp(name, "cal-", 4) != 0) continue;
    bool live = false;
    for (uint8_t i = 0; i < kept && !live; i++) live = strcmp(keep[i].data(), name) == 0;
    if (!live) Storage.remove((dir + "/" + name).c_str());
  }
  handle.close();
}

}  // namespace bookorbit
