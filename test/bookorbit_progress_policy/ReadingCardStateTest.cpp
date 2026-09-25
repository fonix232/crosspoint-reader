#include <gtest/gtest.h>

#include "src/util/ReadingCardState.h"

TEST(ReadingCardState, ChangedPositionIncludingBackwardsIsPending) {
  bookorbit::LocalProgressState local{true, 4, 100};
  bookorbit::AckState ack{true, 4, 100};
  EXPECT_EQ(readingCardSync(true, true, true, false, false, false, true, local, ack), ReadingCardSync::Synced);
  local.key = 90;
  EXPECT_EQ(readingCardSync(true, true, true, false, false, false, true, local, ack), ReadingCardSync::Pending);
  ack.hasLocalAck = false;
  EXPECT_EQ(readingCardSync(true, true, true, false, false, false, true, local, ack), ReadingCardSync::Pending);
}

TEST(ReadingCardState, ConflictSurvivesBackgroundRetryAndFailure) {
  bookorbit::LocalProgressState local{true, 4, 100};
  bookorbit::AckState ack{true, 4, 100};
  EXPECT_EQ(readingCardSync(true, true, true, true, true, true, true, local, ack), ReadingCardSync::Conflict);
  EXPECT_EQ(readingCardSync(true, true, true, false, true, true, true, local, ack), ReadingCardSync::Syncing);
  local.key = 101;
  EXPECT_EQ(readingCardSync(true, true, true, false, false, true, true, local, ack), ReadingCardSync::Failed);
}

TEST(ReadingCardState, MissingPositionOrUnreadableIndexCannotClaimSynced) {
  bookorbit::LocalProgressState local;
  bookorbit::AckState ack{true, 0, 0};
  EXPECT_EQ(readingCardSync(true, true, true, false, false, false, true, local, ack), ReadingCardSync::Unknown);
  EXPECT_EQ(readingCardSync(true, true, false, false, false, false, false, local, ack), ReadingCardSync::Unknown);
  EXPECT_EQ(readingCardSync(true, true, false, false, false, false, true, local, ack), ReadingCardSync::Local);
  EXPECT_EQ(readingCardSync(true, false, true, false, false, false, true, local, ack), ReadingCardSync::Disabled);
  EXPECT_EQ(readingCardSync(false, true, false, false, false, false, true, local, ack), ReadingCardSync::Local);
}

TEST(ReadingCardState, SingleBookAcknowledgementClearsErrorWithoutMarkingAccountSynced) {
  bookorbit::LocalProgressState local{true, 4, 100};
  bookorbit::AckState ack{true, 4, 90};
  EXPECT_EQ(readingCardSync(true, true, true, false, false, true, true, local, ack), ReadingCardSync::Failed);
  ack.ackKey = local.key;
  EXPECT_EQ(readingCardSync(true, true, true, false, false, true, true, local, ack), ReadingCardSync::Synced);
  EXPECT_EQ(readingCardSync(true, true, true, true, false, true, true, local, ack), ReadingCardSync::Conflict);
}
