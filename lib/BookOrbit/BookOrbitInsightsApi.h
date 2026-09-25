#pragma once

#include <ArduinoJson.h>

#include "BookOrbitInsights.h"

namespace bookorbit {
// Validates each official response before adding it to a bounded local snapshot.
bool addInsightsWidget(JsonDocument& snapshot, const char* section, const std::string& body);
bool addInsightsBuckets(JsonDocument& snapshot, bool hours, const std::string& body);
void addInsightsHeatmap(JsonDocument& snapshot, const InsightsSummary& summary);
void serializeInsightsCalendar(const InsightsCalendar& calendar, std::string& body);
// Number of UTC days needed to include the first of a requested month.
uint16_t insightsHeatmapDays(uint16_t year, uint8_t month, uint32_t now);

// Network responses can cover years. Only the requested month and a rolling
// 30-day overview are retained; memory does not grow with the history window.
class InsightsHeatmapParser {
 public:
  InsightsHeatmapParser(InsightsCalendar& calendar, InsightsSummary* summary = nullptr);
  void feed(const char* data, size_t length);
  bool finish(uint16_t expectedDays = 0);

 private:
  static void key(void*, const char*, size_t);
  static void string(void*, const char*, size_t);
  static void number(void*, const char*, size_t);
  static void objectStart(void*);
  static void objectEnd(void*);
  static void arrayStart(void*);
  static void arrayEnd(void*);
  static void invalid(void*);
  StreamingJsonParser parser;
  InsightsCalendar& calendar;
  InsightsSummary* summary;
  char field[24] = {};
  char date[11] = {};
  char previous[11] = {};
  uint32_t seconds = 0;
  uint16_t events = 0;
  uint32_t sources[INSIGHTS_SOURCES] = {};
  uint32_t recent[30] = {};
  uint16_t count = 0;
  uint8_t depth = 0;
  uint8_t arrays = 0;
  bool source = false;
  bool failed = false;
  bool closed = false;
  bool sawSeconds = false;
};
}  // namespace bookorbit
