#include "BookOrbitInsights.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "BookOrbitCatalogFormat.h"

namespace bookorbit {

namespace {
void copyKey(const char* value, const size_t len, char* out, const size_t cap) {
  const size_t n = len < cap - 1 ? len : cap - 1;
  memcpy(out, value, n);
  out[n] = '\0';
}

bool keyIs(const char* key, const char* name) { return strcmp(key, name) == 0; }

uint32_t toU32(const char* value, const size_t len) {
  char buf[24];
  copyKey(value, len, buf, sizeof(buf));
  const double d = strtod(buf, nullptr);
  if (d <= 0) return 0;
  if (d >= 4294967295.0) return 4294967295u;
  return static_cast<uint32_t>(d + 0.5);
}

uint16_t toU16(const char* value, const size_t len) {
  const uint32_t v = toU32(value, len);
  return v > 65535u ? 65535u : static_cast<uint16_t>(v);
}

uint8_t toScore(const char* value, const size_t len) {
  const uint32_t v = toU32(value, len);
  return v > 100u ? 100u : static_cast<uint8_t>(v);
}

// "YYYY-MM-DD" -> day of month, 0 when malformed.
uint8_t dayOfMonthOf(const char* date) {
  if (strlen(date) != 10 || date[4] != '-' || date[7] != '-') return 0;
  const int day = atoi(date + 8);
  return day >= 1 && day <= 31 ? static_cast<uint8_t>(day) : 0;
}

int sourceIndex(const char* key) {
  if (keyIs(key, "bookorbit")) return static_cast<int>(InsightsSource::BookOrbit);
  if (keyIs(key, "koreader")) return static_cast<int>(InsightsSource::KOReader);
  if (keyIs(key, "kobo")) return static_cast<int>(InsightsSource::Kobo);
  return -1;
}
}  // namespace

bool InsightsSummary::speedAvailable() const { return speedLabel[0] != '\0' && strcmp(speedLabel, "N/A") != 0; }

uint32_t InsightsCalendar::totalSeconds() const {
  uint32_t total = 0;
  for (uint8_t i = 0; i < dayCount && i < 31; i++) total += seconds[i];
  return total;
}

uint8_t InsightsCalendar::activeDays() const {
  uint8_t active = 0;
  for (uint8_t i = 0; i < dayCount && i < 31; i++) {
    if (seconds[i] > 0) active++;
  }
  return active;
}

uint8_t InsightsCalendar::busiestDay() const {
  uint8_t best = 0;
  uint32_t most = 0;
  for (uint8_t i = 0; i < dayCount && i < 31; i++) {
    if (seconds[i] > most) {
      most = seconds[i];
      best = static_cast<uint8_t>(i + 1);
    }
  }
  return best;
}

uint8_t InsightsHabits::bestWeekday() const {
  return static_cast<uint8_t>(std::max_element(weekday, weekday + 7) - weekday);
}

uint8_t InsightsHabits::peakHour() const { return static_cast<uint8_t>(std::max_element(hour, hour + 24) - hour); }

// --- parser hooks ------------------------------------------------------------------
// One static hook set per parser, dispatching on the object path. The response
// nests at most two levels (section -> field), plus arrays of scalars or of
// flat objects, so a section name and the last key are all the state needed.

struct InsightsParserHooks {
  // Summary ---------------------------------------------------------------------
  static void sumKey(void* ctx, const char* key, const size_t len) {
    auto* p = static_cast<InsightsSummaryParser*>(ctx);
    if (p->depth == 1) {
      copyKey(key, len, p->section, sizeof(p->section));
      p->key[0] = '\0';
    } else {
      copyKey(key, len, p->key, sizeof(p->key));
    }
  }
  static void sumString(void* ctx, const char* value, const size_t len) {
    auto* p = static_cast<InsightsSummaryParser*>(ctx);
    InsightsSummary& o = p->out;
    if (p->depth == 1) {
      if (keyIs(p->section, "generatedAt")) o.generatedAt = parseIso8601Utc(std::string_view(value, len));
      if (keyIs(p->section, "daysTimezone")) copyKey(value, len, o.daysTimezone, sizeof(o.daysTimezone));
      return;
    }
    if (p->depth == 2 && keyIs(p->section, "dna")) {
      char* dest = nullptr;
      if (keyIs(p->key, "archetype")) dest = o.archetype;
      if (keyIs(p->key, "lengthLabel")) dest = o.lengthLabel;
      if (keyIs(p->key, "varietyLabel")) dest = o.varietyLabel;
      if (keyIs(p->key, "rhythmLabel")) dest = o.rhythmLabel;
      if (keyIs(p->key, "timeLabel")) dest = o.timeLabel;
      if (keyIs(p->key, "speedLabel")) dest = o.speedLabel;
      if (dest) copyUtf8Bounded(std::string_view(value, len), dest, INSIGHTS_LABEL_BYTES - 1);
      return;
    }
    if (p->depth == 2 && keyIs(p->section, "projection") && keyIs(p->key, "trend")) {
      o.trend = len == 2 && strncmp(value, "up", 2) == 0 ? 1 : len == 4 && strncmp(value, "down", 4) == 0 ? -1 : 0;
      return;
    }
    if (p->inWeek && p->depth == 2 && keyIs(p->key, "date") && p->weekIndex >= 0 && p->weekIndex < 7) {
      copyKey(value, len, o.week[p->weekIndex].date, sizeof(o.week[p->weekIndex].date));
    }
  }
  static void sumNumber(void* ctx, const char* value, const size_t len) {
    auto* p = static_cast<InsightsSummaryParser*>(ctx);
    InsightsSummary& o = p->out;
    if (p->depth == 1) {
      if (keyIs(p->section, "schemaVersion")) {
        p->sawVersion = true;
        if (toU32(value, len) != INSIGHTS_SCHEMA_VERSION) p->failed = true;
      }
      return;
    }
    if (p->depth != 2) return;
    const char* k = p->key;
    if (keyIs(p->section, "goal")) {
      if (keyIs(k, "goalBooks")) o.goalBooks = toU16(value, len);
      if (keyIs(k, "completedBooks")) o.goalCompleted = toU16(value, len);
      if (keyIs(k, "year")) o.goalYear = toU16(value, len);
    } else if (keyIs(p->section, "streak")) {
      if (keyIs(k, "currentStreak")) o.currentStreak = toU16(value, len);
      if (keyIs(k, "longestStreak")) o.longestStreak = toU16(value, len);
    } else if (keyIs(p->section, "dna")) {
      if (keyIs(k, "lengthScore")) o.lengthScore = toScore(value, len);
      if (keyIs(k, "varietyScore")) o.varietyScore = toScore(value, len);
      if (keyIs(k, "rhythmScore")) o.rhythmScore = toScore(value, len);
      if (keyIs(k, "timeScore")) o.timeScore = toScore(value, len);
      if (keyIs(k, "speedScore")) o.speedScore = toScore(value, len);
      if (keyIs(k, "booksAnalyzed")) o.booksAnalyzed = toU16(value, len);
    } else if (keyIs(p->section, "projection")) {
      if (keyIs(k, "projectedBooks")) o.projectedBooks = toU16(value, len);
      if (keyIs(k, "booksCompletedYtd")) o.booksCompletedYtd = toU16(value, len);
      if (keyIs(k, "daysRemaining")) o.daysRemaining = toU16(value, len);
    } else if (keyIs(p->section, "totals")) {
      if (keyIs(k, "last7DaysSeconds")) o.last7DaysSeconds = toU32(value, len);
      if (keyIs(k, "last30DaysSeconds")) o.last30DaysSeconds = toU32(value, len);
      if (keyIs(k, "last7DaysActive"))
        o.last7DaysActive = static_cast<uint8_t>(std::min<uint32_t>(toU32(value, len), 7));
      if (keyIs(k, "last30DaysActive"))
        o.last30DaysActive = static_cast<uint8_t>(std::min<uint32_t>(toU32(value, len), 30));
    } else if (keyIs(p->section, "library")) {
      if (keyIs(k, "trackedBooks")) o.trackedBooks = toU16(value, len);
      if (keyIs(k, "startedBooks")) o.startedBooks = toU16(value, len);
      if (keyIs(k, "inProgressBooks")) o.inProgressBooks = toU16(value, len);
      if (keyIs(k, "completedBooks")) o.completedBooks = toU16(value, len);
    } else if (p->inWeek && keyIs(k, "readingSeconds") && p->weekIndex >= 0 && p->weekIndex < 7) {
      o.week[p->weekIndex].seconds = toU32(value, len);
    }
  }
  static void sumBool(void* ctx, const bool value) {
    auto* p = static_cast<InsightsSummaryParser*>(ctx);
    if (p->inSevenDays && p->sevenIndex < 7) {
      if (value) p->out.lastSevenDays |= static_cast<uint8_t>(1u << p->sevenIndex);
      p->sevenIndex++;
    }
  }
  static void sumNull(void*) {}  // goalBooks null stays 0 = no goal
  static void sumObjStart(void* ctx) {
    auto* p = static_cast<InsightsSummaryParser*>(ctx);
    p->depth++;
    if (p->inWeek && p->depth == 2) {
      p->weekIndex++;
      if (p->weekIndex < 7) p->out.weekCount = static_cast<uint8_t>(p->weekIndex + 1);
    }
  }
  static void sumObjEnd(void* ctx) {
    auto* p = static_cast<InsightsSummaryParser*>(ctx);
    if (p->depth > 0) p->depth--;
  }
  static void sumArrStart(void* ctx) {
    auto* p = static_cast<InsightsSummaryParser*>(ctx);
    if (p->depth == 1 && keyIs(p->section, "week")) {
      p->inWeek = true;
      p->weekIndex = -1;
    } else if (p->depth == 2 && keyIs(p->section, "streak") && keyIs(p->key, "lastSevenDays")) {
      p->inSevenDays = true;
      p->sevenIndex = 0;
      p->out.lastSevenDays = 0;
    }
  }
  static void sumArrEnd(void* ctx) {
    auto* p = static_cast<InsightsSummaryParser*>(ctx);
    if (p->inWeek && p->depth == 1) p->inWeek = false;
    if (p->inSevenDays) p->inSevenDays = false;
  }

  // Calendar --------------------------------------------------------------------
  static void calKey(void* ctx, const char* key, const size_t len) {
    auto* p = static_cast<InsightsCalendarParser*>(ctx);
    copyKey(key, len, p->key, sizeof(p->key));
    if (p->depth == 1 && keyIs(p->key, "days")) p->inDays = true;
  }
  static void calString(void* ctx, const char* value, const size_t len) {
    auto* p = static_cast<InsightsCalendarParser*>(ctx);
    InsightsCalendar& o = p->out;
    if (p->depth == 1) {
      if (keyIs(p->key, "generatedAt")) o.generatedAt = parseIso8601Utc(std::string_view(value, len));
      if (keyIs(p->key, "daysTimezone")) copyKey(value, len, o.daysTimezone, sizeof(o.daysTimezone));
      if (keyIs(p->key, "dataThrough")) copyKey(value, len, o.dataThrough, sizeof(o.dataThrough));
      if (keyIs(p->key, "month") && len == 7 && value[4] == '-') {
        char buf[8];
        copyKey(value, len, buf, sizeof(buf));
        o.year = static_cast<uint16_t>(atoi(buf));
        o.month = static_cast<uint8_t>(atoi(buf + 5));
      }
      return;
    }
    if (p->inDays && p->depth == 2 && keyIs(p->key, "day")) copyKey(value, len, p->pendingDay, sizeof(p->pendingDay));
  }
  static void calNumber(void* ctx, const char* value, const size_t len) {
    auto* p = static_cast<InsightsCalendarParser*>(ctx);
    if (p->depth == 1) {
      if (keyIs(p->key, "schemaVersion")) {
        p->sawVersion = true;
        if (toU32(value, len) != INSIGHTS_SCHEMA_VERSION) p->failed = true;
      }
      return;
    }
    if (!p->inDays) return;
    if (p->depth == 2) {
      if (keyIs(p->key, "readingSeconds")) p->pendingSeconds = toU32(value, len);
      if (keyIs(p->key, "eventsCount")) p->pendingEvents = toU16(value, len);
    } else if (p->depth == 3 && p->inSource) {
      const int index = sourceIndex(p->key);
      if (index >= 0) p->pendingSources[index] = toU32(value, len);
    }
  }
  static void calBool(void*, bool) {}
  static void calNull(void*) {}
  static void calObjStart(void* ctx) {
    auto* p = static_cast<InsightsCalendarParser*>(ctx);
    p->depth++;
    if (p->inDays && p->depth == 2) {
      p->pendingDay[0] = '\0';
      p->pendingSeconds = 0;
      p->pendingEvents = 0;
      memset(p->pendingSources, 0, sizeof(p->pendingSources));
    } else if (p->inDays && p->depth == 3 && keyIs(p->key, "bySource")) {
      p->inSource = true;
    }
  }
  static void calObjEnd(void* ctx) {
    auto* p = static_cast<InsightsCalendarParser*>(ctx);
    if (p->inDays && p->depth == 3) p->inSource = false;
    if (p->inDays && p->depth == 2) {
      // Rows land by their own date, so a gap in the list never shifts days.
      const uint8_t day = dayOfMonthOf(p->pendingDay);
      if (day == 0) {
        p->failed = true;
      } else {
        InsightsCalendar& o = p->out;
        o.seconds[day - 1] = p->pendingSeconds;
        o.events[day - 1] = p->pendingEvents;
        memcpy(o.bySource[day - 1], p->pendingSources, sizeof(p->pendingSources));
        if (day > o.dayCount) o.dayCount = day;
      }
    }
    if (p->depth > 0) p->depth--;
  }
  static void calArrStart(void*) {}
  static void calArrEnd(void* ctx) {
    auto* p = static_cast<InsightsCalendarParser*>(ctx);
    if (p->depth == 1) p->inDays = false;
  }

  // Habits ----------------------------------------------------------------------
  static void habKey(void* ctx, const char* key, const size_t len) {
    auto* p = static_cast<InsightsHabitsParser*>(ctx);
    copyKey(key, len, p->key, sizeof(p->key));
  }
  static void habString(void* ctx, const char* value, const size_t len) {
    auto* p = static_cast<InsightsHabitsParser*>(ctx);
    InsightsHabits& o = p->out;
    if (p->depth != 1) return;
    if (keyIs(p->key, "generatedAt")) o.generatedAt = parseIso8601Utc(std::string_view(value, len));
    if (keyIs(p->key, "weekdaysTimezone")) copyKey(value, len, o.weekdaysTimezone, sizeof(o.weekdaysTimezone));
    if (keyIs(p->key, "hoursTimezone")) copyKey(value, len, o.hoursTimezone, sizeof(o.hoursTimezone));
  }
  static void habNumber(void* ctx, const char* value, const size_t len) {
    auto* p = static_cast<InsightsHabitsParser*>(ctx);
    InsightsHabits& o = p->out;
    if (p->inWeekdays) {
      if (p->arrayIndex < 7) o.weekday[p->arrayIndex] = toU32(value, len);
      p->arrayIndex++;
      return;
    }
    if (p->inHours) {
      if (p->arrayIndex < 24) o.hour[p->arrayIndex] = toU32(value, len);
      p->arrayIndex++;
      return;
    }
    if (p->depth != 1) return;
    if (keyIs(p->key, "schemaVersion")) {
      p->sawVersion = true;
      if (toU32(value, len) != INSIGHTS_SCHEMA_VERSION) p->failed = true;
    }
    if (keyIs(p->key, "days")) o.days = toU16(value, len);
  }
  static void habBool(void*, bool) {}
  static void habNull(void*) {}
  static void habObjStart(void* ctx) { static_cast<InsightsHabitsParser*>(ctx)->depth++; }
  static void habObjEnd(void* ctx) {
    auto* p = static_cast<InsightsHabitsParser*>(ctx);
    if (p->depth > 0) p->depth--;
  }
  static void habArrStart(void* ctx) {
    auto* p = static_cast<InsightsHabitsParser*>(ctx);
    if (p->depth != 1) return;
    p->arrayIndex = 0;
    if (keyIs(p->key, "weekdaySeconds")) p->inWeekdays = true;
    if (keyIs(p->key, "hourSeconds")) p->inHours = true;
  }
  static void habArrEnd(void* ctx) {
    auto* p = static_cast<InsightsHabitsParser*>(ctx);
    if (p->inWeekdays && p->arrayIndex != 7) p->failed = true;
    if (p->inHours && p->arrayIndex != 24) p->failed = true;
    p->inWeekdays = false;
    p->inHours = false;
  }
};

InsightsSummaryParser::InsightsSummaryParser(InsightsSummary& out)
    : parser(JsonCallbacks{
          this, &InsightsParserHooks::sumKey, &InsightsParserHooks::sumString, &InsightsParserHooks::sumNumber,
          &InsightsParserHooks::sumBool, &InsightsParserHooks::sumNull, &InsightsParserHooks::sumObjStart,
          &InsightsParserHooks::sumObjEnd, &InsightsParserHooks::sumArrStart, &InsightsParserHooks::sumArrEnd}),
      out(out) {
  out = InsightsSummary{};
}

void InsightsSummaryParser::feed(const char* data, const size_t len) { parser.feed(data, len); }

bool InsightsSummaryParser::finish() {
  out.valid = !failed && !parser.hasError() && sawVersion && depth == 0;
  return out.valid;
}

InsightsCalendarParser::InsightsCalendarParser(InsightsCalendar& out)
    : parser(JsonCallbacks{
          this, &InsightsParserHooks::calKey, &InsightsParserHooks::calString, &InsightsParserHooks::calNumber,
          &InsightsParserHooks::calBool, &InsightsParserHooks::calNull, &InsightsParserHooks::calObjStart,
          &InsightsParserHooks::calObjEnd, &InsightsParserHooks::calArrStart, &InsightsParserHooks::calArrEnd}),
      out(out) {
  out = InsightsCalendar{};
}

void InsightsCalendarParser::feed(const char* data, const size_t len) { parser.feed(data, len); }

bool InsightsCalendarParser::finish() {
  out.valid =
      !failed && !parser.hasError() && sawVersion && depth == 0 && out.year != 0 && out.month >= 1 && out.month <= 12;
  return out.valid;
}

InsightsHabitsParser::InsightsHabitsParser(InsightsHabits& out)
    : parser(JsonCallbacks{
          this, &InsightsParserHooks::habKey, &InsightsParserHooks::habString, &InsightsParserHooks::habNumber,
          &InsightsParserHooks::habBool, &InsightsParserHooks::habNull, &InsightsParserHooks::habObjStart,
          &InsightsParserHooks::habObjEnd, &InsightsParserHooks::habArrStart, &InsightsParserHooks::habArrEnd}),
      out(out) {
  out = InsightsHabits{};
}

void InsightsHabitsParser::feed(const char* data, const size_t len) { parser.feed(data, len); }

bool InsightsHabitsParser::finish() {
  out.valid = !failed && !parser.hasError() && sawVersion && depth == 0;
  return out.valid;
}

// --- helpers --------------------------------------------------------------------------

void formatDurationShort(const uint32_t seconds, char* out, const size_t cap) {
  const uint32_t minutes = (seconds + 30) / 60;
  if (minutes >= 60) {
    snprintf(out, cap, "%luh %02lum", static_cast<unsigned long>(minutes / 60),
             static_cast<unsigned long>(minutes % 60));
  } else {
    snprintf(out, cap, "%lum", static_cast<unsigned long>(minutes));
  }
}

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return leap ? 29 : 28;
  }
  return days[month - 1];
}

uint8_t weekdayOfFirst(const uint16_t year, const uint8_t month) {
  // Zeller-style congruence (Sakamoto), Sunday = 0.
  static const int8_t offsets[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = year;
  if (month < 3) y--;
  return static_cast<uint8_t>((y + y / 4 - y / 100 + y / 400 + offsets[month - 1] + 1) % 7);
}

}  // namespace bookorbit
