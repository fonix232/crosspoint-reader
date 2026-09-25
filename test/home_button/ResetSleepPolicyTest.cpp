#include <gtest/gtest.h>

#include "util/ResetSleepPolicy.h"

using reset_sleep::BootCause;
using reset_sleep::shouldSleep;

TEST(ResetSleepPolicy, FirstBootAndSleepWakeBootNormally) {
  EXPECT_FALSE(shouldSleep(false, BootCause::External, false));
}

TEST(ResetSleepPolicy, HardwareResetWhileAwakeSleeps) { EXPECT_TRUE(shouldSleep(true, BootCause::External, false)); }

TEST(ResetSleepPolicy, RepeatedButtonResetsAlternate) {
  bool savedAwake = false;
  for (int boot = 0; boot < 6; ++boot) {
    const bool sleep = shouldSleep(savedAwake, BootCause::External, false);
    EXPECT_EQ(sleep, boot % 2 == 1);
    savedAwake = !sleep;
  }
}

TEST(ResetSleepPolicy, SoftwareAndMaintenanceRestartsRemainAwake) {
  EXPECT_FALSE(shouldSleep(true, BootCause::Software, false));
  EXPECT_FALSE(shouldSleep(true, BootCause::External, true));
}

TEST(ResetSleepPolicy, FaultsAndOtherResetCausesRemainAwake) {
  EXPECT_FALSE(shouldSleep(true, BootCause::Fault, false));
  EXPECT_FALSE(shouldSleep(true, BootCause::Other, false));
}

TEST(ResetSleepPolicy, TapWakeKeepsTheSavedSleepResumeRoute) {
  EXPECT_TRUE(reset_sleep::isSleepWake(false, BootCause::Software, false, true));
  EXPECT_TRUE(reset_sleep::isSleepWake(false, BootCause::External, false, false));
  EXPECT_FALSE(reset_sleep::isSleepWake(false, BootCause::Software, false, false));
  EXPECT_FALSE(reset_sleep::isSleepWake(true, BootCause::Software, false, true));
  EXPECT_FALSE(reset_sleep::isSleepWake(false, BootCause::Fault, false, true));
  EXPECT_FALSE(reset_sleep::isSleepWake(false, BootCause::Software, true, true));
}
