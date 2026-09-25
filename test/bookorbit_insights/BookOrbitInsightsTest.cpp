#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "BookOrbitInsights.h"

using namespace bookorbit;

namespace {

const char* SUMMARY_JSON = R"({
  "schemaVersion": 2,
  "generatedAt": "2026-04-08T10:00:00.000Z",
  "daysTimezone": "Europe/Budapest",
  "goal": {"goalBooks": 20, "completedBooks": 7, "year": 2026},
  "streak": {"currentStreak": 3, "longestStreak": 12, "lastSevenDays": [true, false, true, true, true, false, true]},
  "dna": {"archetype": "Marathoner", "lengthScore": 80.4, "varietyScore": 40, "rhythmScore": 60, "timeScore": 30,
          "speedScore": 0, "lengthLabel": "Epic", "varietyLabel": "Focused", "rhythmLabel": "Steady",
          "timeLabel": "Night owl", "speedLabel": "N/A", "booksAnalyzed": 5},
  "projection": {"projectedBooks": 22, "projectedPages": 6000, "projectedHours": 120, "booksCompletedYtd": 7,
                 "daysRemaining": 267, "trend": "up"},
  "week": [{"date": "2026-04-02", "readingSeconds": 600}, {"date": "2026-04-03", "readingSeconds": 0},
           {"date": "2026-04-04", "readingSeconds": 1800}, {"date": "2026-04-05", "readingSeconds": 0},
           {"date": "2026-04-06", "readingSeconds": 300}, {"date": "2026-04-07", "readingSeconds": 0},
           {"date": "2026-04-08", "readingSeconds": 900}],
  "totals": {"last7DaysSeconds": 3600, "last30DaysSeconds": 5400, "last7DaysActive": 4, "last30DaysActive": 8},
  "library": {"trackedBooks": 40, "startedBooks": 12, "inProgressBooks": 3, "completedBooks": 9, "meanProgressPercent": 51.2}
})";

const char* CALENDAR_JSON = R"({
  "schemaVersion": 2,
  "generatedAt": "2026-04-08T10:00:00.000Z",
  "month": "2026-03",
  "daysTimezone": "UTC",
  "dataThrough": "2026-04-08",
  "days": [
    {"day": "2026-03-01", "readingSeconds": 0, "eventsCount": 0, "bySource": {"bookorbit": 0, "koreader": 0, "kobo": 0}},
    {"day": "2026-03-05", "readingSeconds": 300, "eventsCount": 1, "bySource": {"bookorbit": 0, "koreader": 300, "kobo": 0}},
    {"day": "2026-03-31", "readingSeconds": 7200, "eventsCount": 4, "bySource": {"bookorbit": 3600, "koreader": 0, "kobo": 3600}}
  ]
})";

const char* HABITS_JSON = R"({
  "schemaVersion": 2, "generatedAt": "2026-04-08T10:00:00.000Z", "days": 90,
  "weekdaysTimezone": "UTC", "hoursTimezone": "America/New_York",
  "weekdaySeconds": [100, 0, 0, 0, 0, 0, 600],
  "hourSeconds": [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,700,0]
})";

template <typename Parser>
void feedChunks(Parser& parser, const char* text, const size_t chunk) {
  const size_t len = strlen(text);
  for (size_t pos = 0; pos < len; pos += chunk) parser.feed(text + pos, std::min(chunk, len - pos));
}

}  // namespace

TEST(BookOrbitInsights, ParsesSummary) {
  for (const size_t chunk : {1u, 7u, 4096u}) {
    InsightsSummary s;
    InsightsSummaryParser parser(s);
    feedChunks(parser, SUMMARY_JSON, chunk);
    ASSERT_TRUE(parser.finish()) << "chunk " << chunk;
    EXPECT_EQ(s.generatedAt, 1775642400u);
    EXPECT_STREQ(s.daysTimezone, "Europe/Budapest");
    EXPECT_EQ(s.goalBooks, 20);
    EXPECT_EQ(s.goalCompleted, 7);
    EXPECT_EQ(s.goalYear, 2026);
    EXPECT_EQ(s.currentStreak, 3);
    EXPECT_EQ(s.longestStreak, 12);
    EXPECT_EQ(s.lastSevenDays, 0b1011101);
    EXPECT_STREQ(s.archetype, "Marathoner");
    EXPECT_EQ(s.lengthScore, 80);
    EXPECT_STREQ(s.timeLabel, "Night owl");
    EXPECT_FALSE(s.speedAvailable());
    EXPECT_EQ(s.booksAnalyzed, 5);
    EXPECT_EQ(s.projectedBooks, 22);
    EXPECT_EQ(s.trend, 1);
    ASSERT_EQ(s.weekCount, 7);
    EXPECT_STREQ(s.week[0].date, "2026-04-02");
    EXPECT_EQ(s.week[2].seconds, 1800u);
    EXPECT_EQ(s.week[6].seconds, 900u);
    EXPECT_EQ(s.last7DaysSeconds, 3600u);
    EXPECT_EQ(s.last30DaysActive, 8);
    EXPECT_EQ(s.trackedBooks, 40);
    EXPECT_EQ(s.completedBooks, 9);
  }
}

TEST(BookOrbitInsights, NullGoalMeansNoGoal) {
  const char* json =
      R"({"schemaVersion":2,"goal":{"goalBooks":null,"completedBooks":2,"year":2026},"dna":{"speedLabel":"Brisk"}})";
  InsightsSummary s;
  InsightsSummaryParser parser(s);
  parser.feed(json, strlen(json));
  ASSERT_TRUE(parser.finish());
  EXPECT_EQ(s.goalBooks, 0);
  EXPECT_EQ(s.goalCompleted, 2);
  EXPECT_TRUE(s.speedAvailable());
}

TEST(BookOrbitInsights, RejectsOtherSchemaVersionsAndTruncatedBodies) {
  const char* other = R"({"schemaVersion":1,"goal":{"goalBooks":1}})";
  InsightsSummary s;
  InsightsSummaryParser parser(s);
  parser.feed(other, strlen(other));
  EXPECT_FALSE(parser.finish());
  EXPECT_FALSE(s.valid);

  InsightsSummary t;
  InsightsSummaryParser truncated(t);
  truncated.feed(SUMMARY_JSON, 200);
  EXPECT_FALSE(truncated.finish());

  InsightsSummary u;
  InsightsSummaryParser unversioned(u);
  const char* noVersion = R"({"goal":{"goalBooks":1}})";
  unversioned.feed(noVersion, strlen(noVersion));
  EXPECT_FALSE(unversioned.finish());
}

TEST(BookOrbitInsights, ParsesCalendarByDate) {
  InsightsCalendar c;
  InsightsCalendarParser parser(c);
  feedChunks(parser, CALENDAR_JSON, 13);
  ASSERT_TRUE(parser.finish());
  EXPECT_EQ(c.year, 2026);
  EXPECT_EQ(c.month, 3);
  EXPECT_STREQ(c.dataThrough, "2026-04-08");
  EXPECT_EQ(c.dayCount, 31);
  EXPECT_EQ(c.seconds[4], 300u);
  EXPECT_EQ(c.events[4], 1);
  EXPECT_EQ(c.bySource[4][static_cast<int>(InsightsSource::KOReader)], 300u);
  EXPECT_EQ(c.seconds[30], 7200u);
  EXPECT_EQ(c.bySource[30][static_cast<int>(InsightsSource::Kobo)], 3600u);
  EXPECT_EQ(c.seconds[10], 0u);
  EXPECT_EQ(c.totalSeconds(), 7500u);
  EXPECT_EQ(c.activeDays(), 2);
  EXPECT_EQ(c.busiestDay(), 31);
}

TEST(BookOrbitInsights, PartialMonthStopsAtDataThrough) {
  const char* json = R"({"schemaVersion":2,"month":"2026-04","dataThrough":"2026-04-08","days":[
    {"day":"2026-04-01","readingSeconds":10,"eventsCount":1},{"day":"2026-04-08","readingSeconds":20,"eventsCount":1}]})";
  InsightsCalendar c;
  InsightsCalendarParser parser(c);
  parser.feed(json, strlen(json));
  ASSERT_TRUE(parser.finish());
  EXPECT_EQ(c.dayCount, 8);
  EXPECT_EQ(c.seconds[0], 10u);
  EXPECT_EQ(c.seconds[7], 20u);
}

TEST(BookOrbitInsights, ParsesHabits) {
  InsightsHabits h;
  InsightsHabitsParser parser(h);
  feedChunks(parser, HABITS_JSON, 5);
  ASSERT_TRUE(parser.finish());
  EXPECT_EQ(h.days, 90);
  EXPECT_STREQ(h.hoursTimezone, "America/New_York");
  EXPECT_EQ(h.weekday[0], 100u);
  EXPECT_EQ(h.weekday[6], 600u);
  EXPECT_EQ(h.hour[22], 700u);
  EXPECT_EQ(h.bestWeekday(), 6);
  EXPECT_EQ(h.peakHour(), 22);

  const char* shortArrays = R"({"schemaVersion":2,"days":90,"weekdaySeconds":[1,2,3],"hourSeconds":[]})";
  InsightsHabits bad;
  InsightsHabitsParser badParser(bad);
  badParser.feed(shortArrays, strlen(shortArrays));
  EXPECT_FALSE(badParser.finish());
}

TEST(BookOrbitInsights, DateHelpers) {
  EXPECT_EQ(daysInMonth(2024, 2), 29);
  EXPECT_EQ(daysInMonth(2026, 2), 28);
  EXPECT_EQ(daysInMonth(2100, 2), 28);
  EXPECT_EQ(daysInMonth(2000, 2), 29);
  EXPECT_EQ(daysInMonth(2026, 4), 30);
  EXPECT_EQ(daysInMonth(2026, 13), 0);
  EXPECT_EQ(weekdayOfFirst(2026, 4), 3);  // Wednesday
  EXPECT_EQ(weekdayOfFirst(2026, 3), 0);  // Sunday
  EXPECT_EQ(weekdayOfFirst(2024, 2), 4);  // Thursday
  EXPECT_EQ(weekdayOfFirst(2026, 1), 4);  // Thursday
  EXPECT_EQ(weekdayOfFirst(2000, 1), 6);  // Saturday

  char buf[16];
  formatDurationShort(0, buf, sizeof(buf));
  EXPECT_STREQ(buf, "0m");
  formatDurationShort(29, buf, sizeof(buf));
  EXPECT_STREQ(buf, "0m");
  formatDurationShort(3900, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1h 05m");
  formatDurationShort(36000, buf, sizeof(buf));
  EXPECT_STREQ(buf, "10h 00m");
}

#include "BookOrbitCatalogFormat.h"
#include "BookOrbitInsightsApi.h"

TEST(BookOrbitInsightsApi, OfficialWidgetsProduceReadableSnapshot) {
  JsonDocument doc;
  doc["schemaVersion"] = INSIGHTS_SCHEMA_VERSION;
  EXPECT_TRUE(addInsightsWidget(doc, "goal", R"({"goalBooks":null,"completedBooks":3,"year":2026})"));
  EXPECT_TRUE(addInsightsWidget(
      doc, "streak",
      R"({"currentStreak":2,"longestStreak":8,"lastSevenDays":[false,false,false,false,false,true,true]})"));
  EXPECT_TRUE(addInsightsWidget(
      doc, "dna",
      R"({"archetype":"Explorer","lengthScore":12.5,"varietyScore":80,"rhythmScore":30,"timeScore":25,"speedScore":0,"lengthLabel":"Short","varietyLabel":"Varied","rhythmLabel":"Steady","timeLabel":"Night","speedLabel":"N/A","booksAnalyzed":10})"));
  EXPECT_TRUE(addInsightsWidget(
      doc, "projection",
      R"({"projectedBooks":15,"projectedPages":3000,"projectedHours":90,"booksCompletedYtd":3,"daysRemaining":200,"trend":"up"})"));
  EXPECT_TRUE(addInsightsWidget(
      doc, "library",
      R"({"trackedBooks":80,"startedBooks":20,"inProgressBooks":17,"completedBooks":3,"meanProgressPercent":20.5})"));
  std::string body;
  serializeJson(doc, body);
  InsightsSummary summary;
  InsightsSummaryParser parser(summary);
  parser.feed(body.data(), body.size());
  ASSERT_TRUE(parser.finish());
  EXPECT_EQ(summary.goalBooks, 0);
  EXPECT_EQ(summary.goalCompleted, 3);
  EXPECT_EQ(summary.trackedBooks, 80);
  EXPECT_EQ(summary.lastSevenDays, 96);
  EXPECT_STREQ(summary.archetype, "Explorer");
  EXPECT_EQ(summary.projectedBooks, 15);
  EXPECT_FALSE(summary.speedAvailable());
  EXPECT_FALSE(addInsightsWidget(doc, "library", "{}"));
  EXPECT_FALSE(addInsightsWidget(doc, "goal", R"({"completedBooks":3,"year":2026})"));
  EXPECT_FALSE(addInsightsWidget(doc, "streak", R"({"currentStreak":0,"longestStreak":0,"lastSevenDays":[]})"));
}

TEST(BookOrbitInsightsApi, HabitBucketsUseExplicitIndexes) {
  JsonDocument doc;
  doc["schemaVersion"] = INSIGHTS_SCHEMA_VERSION;
  doc["days"] = 90;
  for (bool hours : {false, true}) {
    JsonDocument rows;
    auto list = rows.to<JsonArray>();
    for (int i = (hours ? 24 : 7) - 1; i >= 0; --i) {
      auto row = list.add<JsonObject>();
      row[hours ? "hour" : "dayOfWeek"] = i;
      row["readingSeconds"] = i * 60;
      row["byFormat"]["EPUB"] = 999;
      row["bySource"]["koreader"] = 999;
    }
    std::string body;
    serializeJson(rows, body);
    ASSERT_TRUE(addInsightsBuckets(doc, hours, body));
    list[0][hours ? "hour" : "dayOfWeek"] = 0;
    body.clear();
    serializeJson(rows, body);
    EXPECT_FALSE(addInsightsBuckets(doc, hours, body));
    list[0][hours ? "hour" : "dayOfWeek"] = hours ? 23 : 6;
    body.clear();
    serializeJson(rows, body);
    ASSERT_TRUE(addInsightsBuckets(doc, hours, body));
  }
  std::string body;
  serializeJson(doc, body);
  InsightsHabits habits;
  InsightsHabitsParser parser(habits);
  parser.feed(body.data(), body.size());
  ASSERT_TRUE(parser.finish());
  EXPECT_EQ(habits.weekday[6], 360);
  EXPECT_EQ(habits.hour[23], 1380);
  EXPECT_STREQ(habits.hoursTimezone, "");
}

TEST(BookOrbitInsightsApi, StreamsLongHistoryRetainingOnlyMonthAndRollingTotals) {
  for (size_t chunk : {1u, 13u, 4096u}) {
    InsightsCalendar calendar;
    calendar.year = 2026;
    calendar.month = 2;
    InsightsSummary summary;
    InsightsHeatmapParser parser(calendar, &summary);
    parser.feed("[", 1);
    for (unsigned month = 1; month <= 3; ++month) {
      for (unsigned day = 1; day <= daysInMonth(2026, month); ++day) {
        char row[240];
        snprintf(row, sizeof(row),
                 "%s{\"day\":\"2026-%02u-%02u\",\"readingSeconds\":60,\"progressDelta\":0.1,\"eventsCount\":1,"
                 "\"bySource\":{\"bookorbit\":10,\"koreader\":20,\"kobo\":30}}",
                 month == 1 && day == 1 ? "" : ",", month, day);
        feedChunks(parser, row, chunk);
      }
    }
    parser.feed("]", 1);
    ASSERT_TRUE(parser.finish());
    EXPECT_TRUE(calendar.valid);
    EXPECT_EQ(calendar.dayCount, 28);
    EXPECT_EQ(calendar.totalSeconds(), 28 * 60);
    EXPECT_STREQ(calendar.dataThrough, "2026-03-31");
    EXPECT_EQ(calendar.bySource[0][1], 20);
    EXPECT_EQ(summary.last30DaysSeconds, 1800);
    EXPECT_EQ(summary.last7DaysSeconds, 420);
    EXPECT_EQ(summary.last30DaysActive, 30);
    EXPECT_STREQ(summary.week[0].date, "2026-03-25");
    std::string body;
    serializeInsightsCalendar(calendar, body);
    InsightsCalendar cached;
    InsightsCalendarParser cacheParser(cached);
    cacheParser.feed(body.data(), body.size());
    ASSERT_TRUE(cacheParser.finish());
    EXPECT_EQ(cached.totalSeconds(), calendar.totalSeconds());
    EXPECT_EQ(cached.bySource[27][2], 30);
  }
}

TEST(BookOrbitInsightsApi, RejectsInvalidHeatmaps) {
  for (const char* body :
       {"{}", "[]", "[", R"([{"day":"2026-01-01"}])", R"([{"day":"2026-01-01","readingSeconds":-1}])",
        R"([{"day":"2026-01-01","readingSeconds":0})",
        R"([{"day":"2026-01-01","readingSeconds":0},{"day":"2026-01-01","readingSeconds":0}])"}) {
    InsightsCalendar calendar;
    InsightsHeatmapParser parser(calendar);
    parser.feed(body, strlen(body));
    EXPECT_FALSE(parser.finish()) << body;
  }
}

TEST(BookOrbitInsightsApi, HistoricalWindowsIncludeLeapDaysAndRespectApiLimits) {
  const auto now = parseIso8601Utc("2026-03-15T12:00:00Z");
  EXPECT_EQ(insightsHeatmapDays(2026, 3, now), 15);
  EXPECT_EQ(insightsHeatmapDays(2026, 2, now), 43);
  EXPECT_EQ(insightsHeatmapDays(2024, 2, parseIso8601Utc("2024-03-01T00:00:00Z")), 30);
  EXPECT_EQ(insightsHeatmapDays(2026, 4, now), 0);
  EXPECT_EQ(insightsHeatmapDays(2010, 1, now), 0);
  EXPECT_EQ(insightsHeatmapDays(2026, 3, 0), 0);
}

TEST(BookOrbitInsightsApi, EmptyReadingAndPartialMonthAreValidButShortWindowsAreNot) {
  const char* body =
      R"([{"readingSeconds":0,"bySource":{"kobo":0,"koreader":0,"bookorbit":0},"day":"2026-04-01","eventsCount":0,"progressDelta":-0.1}])";
  InsightsCalendar calendar;
  calendar.year = 2026;
  calendar.month = 4;
  InsightsSummary summary;
  InsightsHeatmapParser parser(calendar, &summary);
  feedChunks(parser, body, 1);
  ASSERT_TRUE(parser.finish(1));
  EXPECT_TRUE(calendar.valid);
  EXPECT_EQ(calendar.dayCount, 1);
  EXPECT_EQ(summary.last30DaysSeconds, 0);
  EXPECT_EQ(summary.last30DaysActive, 0);
  InsightsCalendar shortCalendar;
  InsightsHeatmapParser shortParser(shortCalendar);
  feedChunks(shortParser, body, 1);
  EXPECT_FALSE(shortParser.finish(30));
}
