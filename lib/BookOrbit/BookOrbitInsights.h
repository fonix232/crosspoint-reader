#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "StreamingJsonParser.h"

namespace bookorbit {

// Fixed-size display records and versioned local snapshots. The official API
// exposes individual dashboard widgets and user-statistics responses; the
// session assembles these into the local cache format, not a server envelope.
// Date buckets are UTC. Peak hours use the account timezone, whose name is
// not included in the response.

inline constexpr uint8_t INSIGHTS_SCHEMA_VERSION = 2;
inline constexpr size_t INSIGHTS_LABEL_BYTES = 32;
inline constexpr size_t INSIGHTS_TZ_BYTES = 40;
inline constexpr size_t INSIGHTS_MAX_JSON = 12 * 1024;

struct InsightsWeekDay {
  char date[11] = {};  // YYYY-MM-DD
  uint32_t seconds = 0;
};

struct InsightsSummary {
  bool valid = false;
  uint32_t generatedAt = 0;  // unix seconds, 0 when absent
  char daysTimezone[INSIGHTS_TZ_BYTES] = {};

  // Goal: goalBooks 0 means "No goal set" (server null).
  uint16_t goalBooks = 0;
  uint16_t goalCompleted = 0;
  uint16_t goalYear = 0;

  uint16_t currentStreak = 0;
  uint16_t longestStreak = 0;
  uint8_t lastSevenDays = 0;  // bit 0 = oldest of the seven, bit 6 = today

  // Reader DNA. booksAnalyzed 0 means the archetype has no evidence.
  char archetype[INSIGHTS_LABEL_BYTES] = {};
  uint8_t lengthScore = 0;
  uint8_t varietyScore = 0;
  uint8_t rhythmScore = 0;
  uint8_t timeScore = 0;
  uint8_t speedScore = 0;
  char lengthLabel[INSIGHTS_LABEL_BYTES] = {};
  char varietyLabel[INSIGHTS_LABEL_BYTES] = {};
  char rhythmLabel[INSIGHTS_LABEL_BYTES] = {};
  char timeLabel[INSIGHTS_LABEL_BYTES] = {};
  char speedLabel[INSIGHTS_LABEL_BYTES] = {};
  uint16_t booksAnalyzed = 0;

  uint16_t projectedBooks = 0;
  uint16_t booksCompletedYtd = 0;
  uint16_t daysRemaining = 0;
  int8_t trend = 0;  // -1 down, 0 stable, 1 up

  uint8_t weekCount = 0;  // oldest first
  InsightsWeekDay week[7];

  uint32_t last7DaysSeconds = 0;
  uint32_t last30DaysSeconds = 0;
  uint8_t last7DaysActive = 0;
  uint8_t last30DaysActive = 0;

  uint16_t trackedBooks = 0;
  uint16_t startedBooks = 0;
  uint16_t inProgressBooks = 0;
  uint16_t completedBooks = 0;

  // BookOrbit reports speed as "N/A" when it has no page data.
  bool speedAvailable() const;
};

enum class InsightsSource : uint8_t { BookOrbit = 0, KOReader = 1, Kobo = 2 };
inline constexpr size_t INSIGHTS_SOURCES = 3;

struct InsightsCalendar {
  bool valid = false;
  uint32_t generatedAt = 0;
  uint16_t year = 0;
  uint8_t month = 0;  // 1..12
  char daysTimezone[INSIGHTS_TZ_BYTES] = {};
  char dataThrough[11] = {};  // last day the aggregates cover
  // Days 1..dayCount are covered (the server omits days after dataThrough).
  uint8_t dayCount = 0;
  uint32_t seconds[31] = {};
  uint16_t events[31] = {};
  uint32_t bySource[31][INSIGHTS_SOURCES] = {};

  uint32_t totalSeconds() const;
  uint8_t activeDays() const;
  // 1-based day with the most reading, 0 when none.
  uint8_t busiestDay() const;
};

struct InsightsHabits {
  bool valid = false;
  uint32_t generatedAt = 0;
  uint16_t days = 0;
  char weekdaysTimezone[INSIGHTS_TZ_BYTES] = {};
  char hoursTimezone[INSIGHTS_TZ_BYTES] = {};
  uint32_t weekday[7] = {};  // Sunday first
  uint32_t hour[24] = {};

  uint8_t bestWeekday() const;  // 0..6, 0 when everything is zero
  uint8_t peakHour() const;
};

// Streaming parsers over local snapshots. Feed the body in chunks; the
// record is complete (and `valid`) after the last chunk when no error was hit.
class InsightsSummaryParser {
 public:
  explicit InsightsSummaryParser(InsightsSummary& out);
  void feed(const char* data, size_t len);
  bool finish();  // returns out.valid

 private:
  friend struct InsightsParserHooks;
  StreamingJsonParser parser;
  InsightsSummary& out;
  uint8_t depth = 0;
  char section[24] = {};
  char key[24] = {};
  bool inWeek = false;
  int8_t weekIndex = -1;
  bool inSevenDays = false;
  uint8_t sevenIndex = 0;
  bool failed = false;
  bool sawVersion = false;
};

class InsightsCalendarParser {
 public:
  explicit InsightsCalendarParser(InsightsCalendar& out);
  void feed(const char* data, size_t len);
  bool finish();

 private:
  friend struct InsightsParserHooks;
  StreamingJsonParser parser;
  InsightsCalendar& out;
  uint8_t depth = 0;
  char key[24] = {};
  bool inDays = false;
  bool inSource = false;
  char pendingDay[11] = {};
  uint32_t pendingSeconds = 0;
  uint16_t pendingEvents = 0;
  uint32_t pendingSources[INSIGHTS_SOURCES] = {};
  bool failed = false;
  bool sawVersion = false;
};

class InsightsHabitsParser {
 public:
  explicit InsightsHabitsParser(InsightsHabits& out);
  void feed(const char* data, size_t len);
  bool finish();

 private:
  friend struct InsightsParserHooks;
  StreamingJsonParser parser;
  InsightsHabits& out;
  uint8_t depth = 0;
  char key[24] = {};
  uint8_t arrayIndex = 0;
  bool inWeekdays = false;
  bool inHours = false;
  bool failed = false;
  bool sawVersion = false;
};

// Helpers shared with the screens.
void formatDurationShort(uint32_t seconds, char* out, size_t cap);  // "1h 05m", "12m", "0m"
// Days in the month; month 1..12.
uint8_t daysInMonth(uint16_t year, uint8_t month);
// 0 = Sunday .. 6 = Saturday for the first of the month (proleptic Gregorian).
uint8_t weekdayOfFirst(uint16_t year, uint8_t month);

}  // namespace bookorbit
