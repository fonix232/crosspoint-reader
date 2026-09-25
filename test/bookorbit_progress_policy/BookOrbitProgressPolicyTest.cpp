#include <gtest/gtest.h>

#include "BookOrbitProgressPolicy.h"

using namespace bookorbit;

namespace {

LocalProgressState localAt(const uint16_t spine, const uint32_t offset) {
  LocalProgressState s;
  s.hasProgress = true;
  s.spine = spine;
  s.key = localProgressKey(true, offset, 0);
  return s;
}

RemoteProgressState remoteAt(const uint32_t ts, const char* locator, const float pct, const bool ours = false) {
  RemoteProgressState r;
  r.found = true;
  r.fromThisDevice = ours;
  r.timestamp = ts;
  r.hash = locatorHash(locator, pct);
  return r;
}

AckState ackOf(const LocalProgressState& local, const RemoteProgressState& remote) {
  AckState a;
  a.hasLocalAck = true;
  a.ackSpine = local.spine;
  a.ackKey = local.key;
  a.hasRemoteAck = true;
  a.ackTimestamp = remote.timestamp;
  a.ackHash = remote.hash;
  return a;
}

}  // namespace

TEST(BookOrbitProgressPolicy, FreshBookWithNoRecordsDoesNothing) {
  EXPECT_EQ(decideSync(LocalProgressState{}, RemoteProgressState{}, AckState{}), SyncDecision::NothingToDo);
}

TEST(BookOrbitProgressPolicy, NeverSyncedLocalProgressUploads) {
  EXPECT_EQ(decideSync(localAt(3, 120), RemoteProgressState{}, AckState{}), SyncDecision::Upload);
}

TEST(BookOrbitProgressPolicy, NeverOpenedBookAppliesRemote) {
  EXPECT_EQ(
      decideSync(LocalProgressState{}, remoteAt(100, "/body/DocFragment[2]/body/p[3]/text().0", 0.2f), AckState{}),
      SyncDecision::ApplyRemote);
}

TEST(BookOrbitProgressPolicy, BothSidesUnchangedSinceAckDoesNothing) {
  const auto local = localAt(3, 120);
  const auto remote = remoteAt(100, "/x", 0.2f);
  EXPECT_EQ(decideSync(local, remote, ackOf(local, remote)), SyncDecision::NothingToDo);
}

TEST(BookOrbitProgressPolicy, LocalMovedServerUnchangedUploadsEvenBackwards) {
  const auto acked = localAt(3, 120);
  const auto remote = remoteAt(100, "/x", 0.2f);
  const auto ack = ackOf(acked, remote);
  EXPECT_EQ(decideSync(localAt(5, 10), remote, ack), SyncDecision::Upload);
  EXPECT_EQ(decideSync(localAt(1, 0), remote, ack), SyncDecision::Upload);  // intentional reread
}

TEST(BookOrbitProgressPolicy, ServerMovedLocalUnchangedAppliesRemote) {
  const auto local = localAt(3, 120);
  const auto acked = remoteAt(100, "/x", 0.2f);
  const auto ack = ackOf(local, acked);
  EXPECT_EQ(decideSync(local, remoteAt(200, "/y", 0.5f), ack), SyncDecision::ApplyRemote);
  // Same timestamp but a different locator is still a change.
  EXPECT_EQ(decideSync(local, remoteAt(100, "/y", 0.2f), ack), SyncDecision::ApplyRemote);
  // Same locator, only the percentage moved (paged documents).
  EXPECT_EQ(decideSync(local, remoteAt(100, "/x", 0.25f), ack), SyncDecision::ApplyRemote);
}

TEST(BookOrbitProgressPolicy, BothMovedIsAConflictNotTheLargerPercentage) {
  const auto acked = localAt(3, 120);
  const auto ackedRemote = remoteAt(100, "/x", 0.2f);
  const auto ack = ackOf(acked, ackedRemote);
  EXPECT_EQ(decideSync(localAt(9, 0), remoteAt(200, "/y", 0.9f), ack), SyncDecision::Conflict);
  EXPECT_EQ(decideSync(localAt(9, 0), remoteAt(200, "/y", 0.01f), ack), SyncDecision::Conflict);
}

TEST(BookOrbitProgressPolicy, OwnUploadEchoIsNotARemoteChange) {
  const auto local = localAt(3, 120);
  AckState ack;
  ack.hasLocalAck = true;
  ack.ackSpine = 3;
  ack.ackKey = local.key;
  // Upload succeeded but the remote acknowledgement was never written.
  EXPECT_EQ(decideSync(local, remoteAt(100, "/x", 0.2f, /*ours=*/true), ack), SyncDecision::NothingToDo);
  // With a local move on top, our own echo must not turn it into a conflict.
  EXPECT_EQ(decideSync(localAt(4, 0), remoteAt(100, "/x", 0.2f, /*ours=*/true), ack), SyncDecision::Upload);
}

TEST(BookOrbitProgressPolicy, LocalKeyDistinguishesOffsetFromPage) {
  EXPECT_NE(localProgressKey(true, 7, 0), localProgressKey(false, 0, 7));
  EXPECT_EQ(localProgressKey(true, 7, 99), localProgressKey(true, 7, 1));  // page ignored with an offset
}

TEST(BookOrbitProgressPolicy, LocatorHashIsStable) {
  EXPECT_EQ(locatorHash("/a", 0.5f), locatorHash("/a", 0.5f));
  EXPECT_NE(locatorHash("/a", 0.5f), locatorHash("/b", 0.5f));
  EXPECT_NE(locatorHash("", 0.5f), locatorHash("", 0.6f));
}
