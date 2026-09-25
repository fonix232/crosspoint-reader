#include <gtest/gtest.h>

#include "BookOrbitSyncSchedule.h"

using bookorbit::progressSyncDue;

TEST(BookOrbitSyncSchedule, DisabledNeverRuns) { EXPECT_FALSE(progressSyncDue(0, 0, 0, false, false, 0)); }

TEST(BookOrbitSyncSchedule, RebootPreservesDeadline) {
  constexpr uint32_t last = 1800000000;
  EXPECT_FALSE(progressSyncDue(30, last + 1799, last, true, false, 0));
  EXPECT_TRUE(progressSyncDue(30, last + 1800, last, true, false, 0));
  EXPECT_TRUE(progressSyncDue(30, last + 86400, last, true, false, 0));
}

TEST(BookOrbitSyncSchedule, MissingOrInvalidClockCatchesUp) {
  EXPECT_TRUE(progressSyncDue(30, 1800000000, 0, false, false, 0));
  EXPECT_TRUE(progressSyncDue(30, 0, 1800000000, true, false, 0));
  EXPECT_TRUE(progressSyncDue(30, 1799999999, 1800000000, true, false, 0));
}

TEST(BookOrbitSyncSchedule, UptimeIgnoresClockCorrectionsAfterAttempt) {
  EXPECT_FALSE(progressSyncDue(30, 0, 1800000000, true, true, 1799999));
  EXPECT_FALSE(progressSyncDue(30, 1900000000, 1800000000, true, true, 1799999));
  EXPECT_TRUE(progressSyncDue(30, 0, 1800000000, true, true, 1800000));
}

TEST(BookOrbitSyncSchedule, FailuresRetryInFiveMinutesIncludingAcrossBoots) {
  EXPECT_FALSE(progressSyncDue(30, 0, 0, false, true, 299999));
  EXPECT_TRUE(progressSyncDue(30, 0, 0, false, true, 300000));
  EXPECT_FALSE(progressSyncDue(30, 1800000299, 1800000000, false, false, 0));
  EXPECT_TRUE(progressSyncDue(30, 1800000300, 1800000000, false, false, 0));
  EXPECT_TRUE(progressSyncDue(1, 0, 0, false, true, 60000));
}

TEST(BookOrbitSyncSchedule, MillisWrapStillUsesElapsedTime) {
  constexpr uint32_t beforeWrap = UINT32_MAX - 100000;
  constexpr uint32_t afterWrap = 1699999;
  EXPECT_TRUE(progressSyncDue(30, 0, 0, true, true, afterWrap - beforeWrap));
}
