#include <Arduino.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <gtest/gtest.h>
#include <sys/time.h>

#include "BookOrbitClock.h"
#include "BookOrbitReadingTracker.h"
#include "BookOrbitSyncState.h"

static time_t systemEpoch = 0;
extern "C" time_t time(time_t* out) noexcept(noexcept(::time(nullptr))) {
  if (out) *out = systemEpoch;
  return systemEpoch;
}
extern "C" int settimeofday(const timeval* value,
                            const struct timezone*) noexcept(noexcept(::settimeofday(nullptr, nullptr))) {
  systemEpoch = value->tv_sec;
  return 0;
}
using namespace bookorbit;
class BookOrbitReadingTrackerTest : public testing::Test {
 protected:
  void SetUp() override {
    fake::reset();
    systemEpoch = 0;
    halClock.available = false;
    halClock.epoch = 0;
    fakeMillis = 1000;
  }
};
TEST_F(BookOrbitReadingTrackerTest, OfflineReadingUsesRtcAfterReboot) {
  halClock.available = true;
  halClock.epoch = 1800000000;
  ReadingTracker tracker;
  tracker.observe("/offline.epub", 0, 3, .2f);
  fakeMillis += 45000;
  tracker.pause();
  ReadingQueue queue("/offline.epub");
  ReadingEvent event;
  unsigned count = 0;
  ASSERT_TRUE(queue.read(&event, 1, count));
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(event.stat.startTime, 1800000000u);
  EXPECT_EQ(event.stat.durationSeconds, 45u);
  // A later boot can upload the event without needing the old uptime anchor.
  event.boot++;
  halClock.available = false;
  EXPECT_TRUE(ReadingTracker::resolveTime(event));
}
TEST_F(BookOrbitReadingTrackerTest, InvalidRtcDoesNotInventTimestamp) {
  halClock.available = true;
  halClock.epoch = 123;
  EXPECT_EQ(nowIfClockValid(), 0u);
  ReadingEvent event;
  event.boot = ReadingTracker::bootId();
  EXPECT_FALSE(ReadingTracker::resolveTime(event));
}
TEST_F(BookOrbitReadingTrackerTest, ProcessClockTakesPrecedenceOverRtc) {
  systemEpoch = 1800000000;
  halClock.available = true;
  halClock.epoch = 1750000000;
  EXPECT_EQ(nowIfClockValid(), 1800000000u);
}
TEST_F(BookOrbitReadingTrackerTest, LateClockMakesRetryTimestampStable) {
  ReadingTracker tracker;
  tracker.observe("/late.epub", 0, 1, .1f);
  fakeMillis = 11000;
  tracker.pause();
  ReadingQueue queue("/late.epub");
  ReadingEvent event;
  unsigned count = 0;
  ASSERT_TRUE(queue.read(&event, 1, count));
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(event.stat.startTime, 0u);
  systemEpoch = 1800000010;
  ASSERT_TRUE(ReadingTracker::resolveTime(event));
  EXPECT_EQ(event.stat.startTime, 1800000000u);
  systemEpoch += 100;
  fakeMillis += 100000;
  ASSERT_TRUE(queue.read(&event, 1, count));
  ASSERT_TRUE(ReadingTracker::resolveTime(event));
  EXPECT_EQ(event.stat.startTime, 1800000000u);
}

TEST_F(BookOrbitReadingTrackerTest, RtcFallbackStillSeedsProcessClockForTls) {
  halClock.available = true;
  halClock.epoch = 1800000000;
  ASSERT_EQ(nowIfClockValid(), 1800000000u);
  ASSERT_EQ(systemEpoch, 0);
  EXPECT_TRUE(ensureSystemClock());
  EXPECT_EQ(systemEpoch, 1800000000);
}
TEST_F(BookOrbitReadingTrackerTest, MissingClockOfflineStaysInvalid) {
  EXPECT_FALSE(ensureSystemClock());
  EXPECT_EQ(nowIfClockValid(), 0u);
}

TEST_F(BookOrbitReadingTrackerTest, FourHoursOutsideReaderAddNoReadingTime) {
  systemEpoch = 1800000000;
  ReadingTracker tracker;
  tracker.observe("/book.epub", 0, 1, .1f);
  fakeMillis += 45000;
  tracker.pause();  // Open menu/control panel/USB or leave the reader.
  fakeMillis += 4 * 60 * 60 * 1000;
  tracker.pause();
  ReadingQueue queue("/book.epub");
  ReadingEvent events[2];
  unsigned count;
  ASSERT_TRUE(queue.read(events, 2, count));
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(events[0].stat.durationSeconds, 45u);
  tracker.observe("/book.epub", 0, 1, .1f);
  fakeMillis += 30000;
  tracker.pause();
  ASSERT_TRUE(queue.read(events, 2, count));
  ASSERT_EQ(count, 2u);
  EXPECT_EQ(events[1].stat.durationSeconds, 30u);
}

TEST_F(BookOrbitReadingTrackerTest, LongIdlePageIsDiscardedInsteadOfClampedAndNeedsPageChange) {
  systemEpoch = 1800000000;
  ReadingTracker tracker;
  tracker.observe("/book.epub", 0, 1, .1f);
  fakeMillis += 4 * 60 * 60 * 1000;
  tracker.pause();
  // A redraw or returning from a panel must not restart this idle page.
  tracker.observe("/book.epub", 0, 1, .1f);
  fakeMillis += 60000;
  tracker.pause();
  ReadingQueue queue("/book.epub");
  ReadingEvent event;
  unsigned count;
  ASSERT_TRUE(queue.read(&event, 1, count));
  EXPECT_EQ(count, 0u);
  tracker.observe("/book.epub", 0, 2, .2f);
  fakeMillis += 45000;
  tracker.pause();
  ASSERT_TRUE(queue.read(&event, 1, count));
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(event.stat.durationSeconds, 45u);
}

TEST_F(BookOrbitReadingTrackerTest, RepeatedRedrawsCannotAccumulateHoursOnSamePage) {
  systemEpoch = 1800000000;
  ReadingTracker tracker;
  for (unsigned i = 0; i < 240; ++i) {
    tracker.observe("/book.epub", 0, 1, .1f);
    fakeMillis += 60000;
    tracker.pause();
  }
  ReadingQueue queue("/book.epub");
  ReadingEvent events[16];
  unsigned count;
  ASSERT_TRUE(queue.read(events, 16, count));
  EXPECT_EQ(count, 5u);
  unsigned total = 0;
  for (unsigned i = 0; i < count; ++i) total += events[i].stat.durationSeconds;
  EXPECT_EQ(total, ReadingTracker::MAX_PAGE_SECONDS);
}

TEST_F(BookOrbitReadingTrackerTest, FiltersRapidFlipsAndAcceptsExactBounds) {
  systemEpoch = 1800000000;
  ReadingTracker tracker;
  tracker.observe("/book.epub", 0, 0, 0);
  fakeMillis += 4000;
  tracker.observe("/book.epub", 0, 1, .1f);
  fakeMillis += 5000;
  tracker.observe("/book.epub", 0, 2, .2f);
  fakeMillis += 300000;
  tracker.observe("/book.epub", 0, 3, .3f);
  fakeMillis += 301000;
  tracker.pause();
  ReadingQueue queue("/book.epub");
  ReadingEvent events[4];
  unsigned count;
  ASSERT_TRUE(queue.read(events, 4, count));
  ASSERT_EQ(count, 2u);
  EXPECT_EQ(events[0].stat.durationSeconds, 5u);
  EXPECT_EQ(events[1].stat.durationSeconds, 300u);
}

TEST_F(BookOrbitReadingTrackerTest, NoOpenedBookMeansNoEventsRegardlessOfUptime) {
  ReadingTracker tracker;
  fakeMillis += 4 * 60 * 60 * 1000;
  tracker.pause();
  tracker.flush();
  EXPECT_TRUE(fake::files.empty());
}

TEST_F(BookOrbitReadingTrackerTest, FailedAnchorWriteOrCloseCanRetryWithoutPartialIdentity) {
  for (bool closeFailure : {false, true}) {
    fake::reset();
    systemEpoch = 1800000010;
    fakeMillis = 11000;
    ReadingEvent event;
    event.boot = ReadingTracker::bootId();
    event.uptime = 1000;
    char anchor[96];
    snprintf(anchor, sizeof(anchor), "/.crosspoint/reading-events/clock-%08lx.bin",
             static_cast<unsigned long>(event.boot));
    if (closeFailure)
      fake::failClosePath = anchor;
    else
      fake::failWrite = 0;
    EXPECT_FALSE(ReadingTracker::resolveTime(event));
    EXPECT_EQ(event.stat.startTime, 0u);
    EXPECT_FALSE(Storage.exists(anchor));
    ASSERT_TRUE(ReadingTracker::resolveTime(event));
    EXPECT_EQ(event.stat.startTime, 1800000000u);
  }
}
