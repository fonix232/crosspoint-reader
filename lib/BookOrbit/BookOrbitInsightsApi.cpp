#include "BookOrbitInsightsApi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "BookOrbitCatalogFormat.h"

namespace bookorbit {
namespace {
bool nonnegative(JsonVariantConst value) { return value.is<double>() && value.as<double>() >= 0; }
bool fields(JsonObjectConst object, const char* const* names, size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (!nonnegative(object[names[i]])) return false;
  return true;
}
uint32_t dayTimestamp(const char* date) {
  if (strlen(date) != 10 || date[4] != '-' || date[7] != '-') return 0;
  const int year = atoi(date), month = atoi(date + 5), day = atoi(date + 8);
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) return 0;
  char iso[21];
  snprintf(iso, sizeof(iso), "%sT00:00:00Z", date);
  return parseIso8601Utc(iso);
}
}  // namespace

bool addInsightsWidget(JsonDocument& snapshot, const char* section, const std::string& body) {
  // One small widget at a time bounds the temporary DOM; never parse a whole
  // heatmap into ArduinoJson on the reader's heap.
  JsonDocument doc;
  if (deserializeJson(doc, body) || !doc.is<JsonObject>()) return false;
  const auto obj = doc.as<JsonObjectConst>();
  bool valid = false;
  if (strcmp(section, "goal") == 0) {
    valid = obj["goalBooks"].isNull() ? !obj["goalBooks"].isUnbound() : nonnegative(obj["goalBooks"]);
    valid = valid && nonnegative(obj["completedBooks"]) && nonnegative(obj["year"]);
  } else if (strcmp(section, "streak") == 0) {
    const auto days = obj["lastSevenDays"].as<JsonArrayConst>();
    valid = nonnegative(obj["currentStreak"]) && nonnegative(obj["longestStreak"]) && days.size() == 7;
    for (auto day : days) valid = valid && day.is<bool>();
  } else if (strcmp(section, "dna") == 0) {
    static constexpr const char* numbers[] = {"lengthScore", "varietyScore", "rhythmScore",
                                              "timeScore",   "speedScore",   "booksAnalyzed"};
    static constexpr const char* labels[] = {"archetype",   "lengthLabel", "varietyLabel",
                                             "rhythmLabel", "timeLabel",   "speedLabel"};
    valid = fields(obj, numbers, 6);
    for (auto name : labels) valid = valid && obj[name].is<const char*>();
  } else if (strcmp(section, "projection") == 0) {
    static constexpr const char* names[] = {"projectedBooks", "booksCompletedYtd", "daysRemaining"};
    const char* trend = obj["trend"] | "";
    valid = fields(obj, names, 3) &&
            (strcmp(trend, "up") == 0 || strcmp(trend, "down") == 0 || strcmp(trend, "stable") == 0);
  } else if (strcmp(section, "library") == 0) {
    static constexpr const char* names[] = {"trackedBooks", "startedBooks", "inProgressBooks", "completedBooks",
                                            "meanProgressPercent"};
    valid = fields(obj, names, 5);
  }
  if (!valid) return false;
  snapshot[section].set(obj);
  return !snapshot.overflowed();
}

bool addInsightsBuckets(JsonDocument& snapshot, bool hours, const std::string& body) {
  JsonDocument doc;
  // Ignore format/source breakdowns, which are not shown in the habits view.
  JsonDocument filter;
  filter[0][hours ? "hour" : "dayOfWeek"] = true;
  filter[0]["readingSeconds"] = true;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) || !doc.is<JsonArray>()) return false;
  const unsigned count = hours ? 24 : 7;
  if (doc.size() != count) return false;
  uint32_t seen = 0;
  auto output = snapshot[hours ? "hourSeconds" : "weekdaySeconds"].to<JsonArray>();
  for (unsigned i = 0; i < count; ++i) output.add(0);
  for (JsonObjectConst row : doc.as<JsonArrayConst>()) {
    auto index = row[hours ? "hour" : "dayOfWeek"];
    if (!index.is<unsigned>() || index.as<unsigned>() >= count || !nonnegative(row["readingSeconds"])) return false;
    const unsigned i = index.as<unsigned>();
    if (seen & (1u << i)) return false;
    seen |= 1u << i;
    output[i] = row["readingSeconds"].as<uint32_t>();
  }
  return !snapshot.overflowed();
}

void addInsightsHeatmap(JsonDocument& snapshot, const InsightsSummary& s) {
  auto totals = snapshot["totals"].to<JsonObject>();
  totals["last7DaysSeconds"] = s.last7DaysSeconds;
  totals["last30DaysSeconds"] = s.last30DaysSeconds;
  totals["last7DaysActive"] = s.last7DaysActive;
  totals["last30DaysActive"] = s.last30DaysActive;
  auto week = snapshot["week"].to<JsonArray>();
  for (unsigned i = 0; i < s.weekCount; ++i) {
    auto day = week.add<JsonObject>();
    day["date"] = s.week[i].date;
    day["readingSeconds"] = s.week[i].seconds;
  }
}

void serializeInsightsCalendar(const InsightsCalendar& c, std::string& body) {
  JsonDocument doc;
  doc["schemaVersion"] = INSIGHTS_SCHEMA_VERSION;
  doc["daysTimezone"] = "UTC";
  doc["dataThrough"] = c.dataThrough;
  char month[8];
  snprintf(month, sizeof(month), "%04u-%02u", c.year, c.month);
  doc["month"] = month;
  auto days = doc["days"].to<JsonArray>();
  static constexpr const char* names[] = {"bookorbit", "koreader", "kobo"};
  for (unsigned i = 0; i < c.dayCount; ++i) {
    auto day = days.add<JsonObject>();
    char date[11];
    snprintf(date, sizeof(date), "%s-%02u", month, i + 1);
    day["day"] = date;
    day["readingSeconds"] = c.seconds[i];
    day["eventsCount"] = c.events[i];
    for (unsigned j = 0; j < INSIGHTS_SOURCES; ++j) day["bySource"][names[j]] = c.bySource[i][j];
  }
  body.clear();
  if (!doc.overflowed()) serializeJson(doc, body);
}

uint16_t insightsHeatmapDays(uint16_t year, uint8_t month, uint32_t now) {
  if (year < 1970 || year > 2100 || month < 1 || month > 12 || now == 0) return 0;
  char first[11];
  snprintf(first, sizeof(first), "%04u-%02u-01", year, month);
  const auto start = dayTimestamp(first);
  if (!start || start / 86400 > now / 86400) return 0;
  const auto days = now / 86400 - start / 86400 + 1;
  return days <= 3650 ? static_cast<uint16_t>(days) : 0;
}

InsightsHeatmapParser::InsightsHeatmapParser(InsightsCalendar& c, InsightsSummary* s)
    : parser(JsonCallbacks{this, key, string, number, nullptr, nullptr, objectStart, objectEnd, arrayStart, arrayEnd,
                           invalid}),
      calendar(c),
      summary(s) {
  c.valid = false;
  if (s) {
    s->weekCount = 0;
    s->last7DaysSeconds = s->last30DaysSeconds = 0;
    s->last7DaysActive = s->last30DaysActive = 0;
  }
}
void InsightsHeatmapParser::feed(const char* data, size_t length) { parser.feed(data, length); }
void InsightsHeatmapParser::invalid(void* ctx) { static_cast<InsightsHeatmapParser*>(ctx)->failed = true; }
void InsightsHeatmapParser::key(void* ctx, const char* value, size_t length) {
  auto& p = *static_cast<InsightsHeatmapParser*>(ctx);
  const auto n = std::min(length, sizeof(p.field) - 1);
  memcpy(p.field, value, n);
  p.field[n] = 0;
}
void InsightsHeatmapParser::string(void* ctx, const char* value, size_t length) {
  auto& p = *static_cast<InsightsHeatmapParser*>(ctx);
  if (p.depth == 1 && strcmp(p.field, "day") == 0 && length == 10) {
    memcpy(p.date, value, 10);
    p.date[10] = 0;
  }
}
void InsightsHeatmapParser::number(void* ctx, const char* value, size_t length) {
  auto& p = *static_cast<InsightsHeatmapParser*>(ctx);
  if (p.depth == 1 && strcmp(p.field, "readingSeconds") != 0 && strcmp(p.field, "eventsCount") != 0) return;
  if (p.depth != 1 && !(p.depth == 2 && p.source)) return;
  char buf[32];
  const auto n = std::min(length, sizeof(buf) - 1);
  memcpy(buf, value, n);
  buf[n] = 0;
  const double v = strtod(buf, nullptr);
  if (!std::isfinite(v) || v < 0 || v > UINT32_MAX) {
    p.failed = true;
    return;
  }
  if (p.depth == 1) {
    if (strcmp(p.field, "readingSeconds") == 0) {
      p.seconds = static_cast<uint32_t>(v);
      p.sawSeconds = true;
    }
    if (strcmp(p.field, "eventsCount") == 0) p.events = static_cast<uint16_t>(std::min(v, 65535.0));
  } else if (p.depth == 2 && p.source) {
    static constexpr const char* names[] = {"bookorbit", "koreader", "kobo"};
    for (unsigned i = 0; i < INSIGHTS_SOURCES; ++i)
      if (strcmp(p.field, names[i]) == 0) p.sources[i] = static_cast<uint32_t>(v);
  }
}
void InsightsHeatmapParser::objectStart(void* ctx) {
  auto& p = *static_cast<InsightsHeatmapParser*>(ctx);
  if (++p.depth == 1) {
    if (p.arrays != 1 || p.closed) p.failed = true;
    p.date[0] = 0;
    p.seconds = 0;
    p.events = 0;
    p.sawSeconds = false;
    memset(p.sources, 0, sizeof(p.sources));
  } else if (p.depth == 2)
    p.source = strcmp(p.field, "bySource") == 0;
}
void InsightsHeatmapParser::objectEnd(void* ctx) {
  auto& p = *static_cast<InsightsHeatmapParser*>(ctx);
  if (p.depth == 2) p.source = false;
  if (p.depth == 1) {
    const uint32_t at = dayTimestamp(p.date);
    if (!p.sawSeconds || !at || (p.count && at != dayTimestamp(p.previous) + 86400) || p.count >= 3650) {
      p.failed = true;
    } else {
      memcpy(p.previous, p.date, sizeof(p.date));
      memcpy(p.calendar.dataThrough, p.date, sizeof(p.date));
      const unsigned day = atoi(p.date + 8);
      if (atoi(p.date) == p.calendar.year && atoi(p.date + 5) == p.calendar.month && day >= 1 &&
          day <= daysInMonth(p.calendar.year, p.calendar.month)) {
        p.calendar.seconds[day - 1] = p.seconds;
        p.calendar.events[day - 1] = p.events;
        memcpy(p.calendar.bySource[day - 1], p.sources, sizeof(p.sources));
        p.calendar.dayCount = day;
      }
      p.recent[p.count % 30] = p.seconds;
      ++p.count;
      if (p.summary) {
        auto& s = *p.summary;
        if (s.weekCount == 7) {
          for (unsigned i = 1; i < 7; ++i) s.week[i - 1] = s.week[i];
        } else
          ++s.weekCount;
        memcpy(s.week[s.weekCount - 1].date, p.date, sizeof(p.date));
        s.week[s.weekCount - 1].seconds = p.seconds;
      }
    }
  }
  if (p.depth)
    --p.depth;
  else
    p.failed = true;
}
void InsightsHeatmapParser::arrayStart(void* ctx) {
  auto& p = *static_cast<InsightsHeatmapParser*>(ctx);
  if (p.arrays++ || p.depth || p.closed) p.failed = true;
}
void InsightsHeatmapParser::arrayEnd(void* ctx) {
  auto& p = *static_cast<InsightsHeatmapParser*>(ctx);
  if (p.arrays != 1 || p.depth) p.failed = true;
  p.arrays = 0;
  p.closed = true;
}
bool InsightsHeatmapParser::finish(uint16_t expectedDays) {
  if (failed || parser.hasError() || !closed || depth || !count || (expectedDays && count != expectedDays))
    return false;
  calendar.valid = calendar.dayCount > 0;
  strcpy(calendar.daysTimezone, "UTC");
  if (summary) {
    for (unsigned i = 0; i < std::min<unsigned>(count, 30); ++i) {
      summary->last30DaysSeconds += recent[i];
      if (recent[i]) ++summary->last30DaysActive;
    }
    for (unsigned i = 0; i < summary->weekCount; ++i) {
      summary->last7DaysSeconds += summary->week[i].seconds;
      if (summary->week[i].seconds) ++summary->last7DaysActive;
    }
  }
  return true;
}
}  // namespace bookorbit
