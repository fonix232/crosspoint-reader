#include <HalStorage.h>
#include <gtest/gtest.h>

#include "BookOrbitInsightsCache.h"

using namespace bookorbit;

TEST(BookOrbitInsightsCache, FailedCloseAndRenamePreserveLastValidSnapshot) {
  for (bool closeFailure : {false, true}) {
    fake::reset();
    const auto path = insightsSummaryPath("test");
    const std::string body = R"({"schemaVersion":2,"goal":{"goalBooks":12}})";
    ASSERT_TRUE(saveInsightsBody(path, body));
    if (closeFailure)
      fake::failClosePath = path + ".tmp";
    else
      fake::failRename = 1;
    EXPECT_FALSE(saveInsightsBody(path, R"({"schemaVersion":2})"));
    EXPECT_TRUE(fake::failureTriggered);
    InsightsSummary summary;
    ASSERT_TRUE(loadInsightsSummary("test", summary));
    EXPECT_EQ(summary.goalBooks, 12);
  }
}

TEST(BookOrbitInsightsCache, ReadFailureCannotValidatePartialSnapshot) {
  fake::reset();
  const auto path = insightsSummaryPath("test");
  ASSERT_TRUE(saveInsightsBody(path, R"({"schemaVersion":2})"));
  fake::failRead = 1;
  InsightsSummary summary;
  EXPECT_FALSE(loadInsightsSummary("test", summary));
  EXPECT_FALSE(summary.valid);
}

TEST(BookOrbitInsightsCache, InterruptedInstallRecoversAndPruneKeepsNewestMonths) {
  fake::reset();
  const auto path = insightsSummaryPath("test");
  ASSERT_TRUE(saveInsightsBody(path, R"({"schemaVersion":2})"));
  ASSERT_TRUE(Storage.rename(path.c_str(), (path + ".bak").c_str()));
  InsightsSummary summary;
  EXPECT_TRUE(loadInsightsSummary("test", summary));
  for (uint8_t month = 1; month <= 12; ++month) fake::add(insightsCalendarPath("test", 2026, month), "{}");
  pruneInsightsCalendars("test", 3);
  for (uint8_t month = 1; month <= 12; ++month)
    EXPECT_EQ(Storage.exists(insightsCalendarPath("test", 2026, month).c_str()), month >= 10);
  EXPECT_TRUE(Storage.exists(path.c_str()));
}
