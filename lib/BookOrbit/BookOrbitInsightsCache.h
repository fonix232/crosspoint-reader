#pragma once

#include <cstdint>
#include <string>

#include "BookOrbitInsights.h"

namespace bookorbit {

// --- on-card cache -------------------------------------------------------------------
// Versioned local snapshots under `<account>/insights/`, re-parsed on screen entry.
std::string insightsDir(const std::string& accountKey);
std::string insightsSummaryPath(const std::string& accountKey);
std::string insightsHabitsPath(const std::string& accountKey);
std::string insightsCalendarPath(const std::string& accountKey, uint16_t year, uint8_t month);

bool loadInsightsSummary(const std::string& accountKey, InsightsSummary& out);
bool loadInsightsHabits(const std::string& accountKey, InsightsHabits& out);
bool loadInsightsCalendar(const std::string& accountKey, uint16_t year, uint8_t month, InsightsCalendar& out);
// Replaces the cached body (staged write). `body` is validated by the caller.
bool saveInsightsBody(const std::string& path, const std::string& body);
// Keeps the `keepMonths` most recent cached months (by name) and removes the rest.
void pruneInsightsCalendars(const std::string& accountKey, uint8_t keepMonths);

}  // namespace bookorbit
