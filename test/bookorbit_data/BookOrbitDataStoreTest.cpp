#include <HalStorage.h>
#include <gtest/gtest.h>

#include <vector>

#include "BookOrbitDataStore.h"
using namespace bookorbit;
namespace {
class BookOrbitDataStoreTest : public testing::Test {
 protected:
  void SetUp() override { fake::reset(); }
  Annotation entry() {
    Annotation a;
    a.key = std::string(32, 'a');
    a.datetime = "2026-09-20 12:00:00";
    a.pos = "/p/1";
    a.text = "quote";
    a.serverId = 1;
    return a;
  }
};
}  // namespace
TEST_F(BookOrbitDataStoreTest, LostAcknowledgementRetainsTheExactBatchAndConcurrentAppends) {
  ReadingQueue queue("/book.epub");
  ReadingEvent first;
  first.stat.startTime = 1750000000;
  first.stat.durationSeconds = 17;
  ASSERT_TRUE(queue.append(&first, 1));
  ReadingEvent batch[2];
  unsigned count = 0;
  ASSERT_TRUE(queue.read(batch, 2, count));
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(batch[0].stat.startTime, first.stat.startTime);
  ReadingEvent second = first;
  second.stat.startTime++;
  ASSERT_TRUE(queue.append(&second, 1));
  ASSERT_TRUE(queue.acknowledge(1));
  ASSERT_TRUE(queue.read(batch, 2, count));
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(batch[0].stat.startTime, second.stat.startTime);
}
TEST_F(BookOrbitDataStoreTest, FailedQueueInstallKeepsOriginalEvents) {
  ReadingQueue queue("/book.epub");
  ReadingEvent a;
  ASSERT_TRUE(queue.append(&a, 1));
  fake::failRename = 1;  // live -> backup succeeds, staging -> live fails
  EXPECT_FALSE(queue.acknowledge(1));
  unsigned count = 0;
  ASSERT_TRUE(queue.read(&a, 1, count));
  EXPECT_EQ(count, 1u);
}
TEST_F(BookOrbitDataStoreTest, MissingHistoryCannotBecomeADeletionInventoryAgain) {
  AnnotationStore store("/annotations");
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(store.put(entry()));
  ASSERT_TRUE(store.markHistory());
  EXPECT_TRUE(store.hasHistory());
  Storage.remove("/annotations/history");
  Storage.remove("/annotations/index");
  AnnotationStore restored("/annotations");
  ASSERT_TRUE(restored.open());
  EXPECT_FALSE(restored.hasHistory());
  ASSERT_TRUE(restored.put(entry()));
  ASSERT_TRUE(restored.markHistory());
  EXPECT_FALSE(restored.hasHistory());
}
TEST_F(BookOrbitDataStoreTest, DeleteLastKeepsDurableIdentityAndHistory) {
  AnnotationStore store("/annotations");
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(store.put(entry()));
  ASSERT_TRUE(store.markHistory());
  ASSERT_TRUE(store.eraseLocal(entry().key));
  AnnotationStore reopened("/annotations");
  ASSERT_TRUE(reopened.open());
  EXPECT_TRUE(reopened.hasHistory());
  Annotation a;
  ASSERT_TRUE(reopened.read(0, a));
  EXPECT_TRUE(a.deleted);
  EXPECT_EQ(a.serverId, 1u);
}
TEST_F(BookOrbitDataStoreTest, InterruptedRecordInstallRestoresPreviousValue) {
  AnnotationStore store("/annotations");
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(store.put(entry()));
  auto changed = entry();
  changed.text = "new";
  fake::failRename = 1;
  EXPECT_FALSE(store.put(changed));
  Annotation a;
  ASSERT_TRUE(store.read(0, a));
  EXPECT_EQ(a.text, "quote");
}
TEST_F(BookOrbitDataStoreTest, CorruptIndexAndRecordNeverLookLikeAnEmptyInventory) {
  AnnotationStore store("/annotations");
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(store.put(entry()));
  fake::add("/annotations/index", "broken");
  EXPECT_FALSE(store.open());
  fake::add("/annotations/index", entry().key);
  fake::add("/annotations/" + entry().key + ".json", "{");
  ASSERT_TRUE(store.open());
  Annotation a;
  EXPECT_FALSE(store.read(0, a));
}

TEST_F(BookOrbitDataStoreTest, ReadingProgressCountsTheWholeQueueNotJustTheUploadBatch) {
  ReadingQueue queue("/book.epub");
  std::vector<ReadingEvent> events(30);
  ASSERT_TRUE(queue.append(events.data(), events.size()));
  unsigned count = 0, queued = 0;
  ASSERT_TRUE(queue.read(events.data(), ReadingQueue::BATCH, count, &queued));
  EXPECT_EQ(count, 25u);
  EXPECT_EQ(queued, 30u);
  // A lost server response removes nothing; an append is reflected in the next count.
  ASSERT_TRUE(queue.append(events.data(), 1));
  ASSERT_TRUE(queue.read(events.data(), ReadingQueue::BATCH, count, &queued));
  EXPECT_EQ(queued, 31u);
  ASSERT_TRUE(queue.acknowledge(25));
  ASSERT_TRUE(queue.read(events.data(), ReadingQueue::BATCH, count, &queued));
  EXPECT_EQ(count, 6u);
  EXPECT_EQ(queued, 6u);
  ASSERT_TRUE(queue.acknowledge(6));
  ASSERT_TRUE(queue.read(events.data(), ReadingQueue::BATCH, count, &queued));
  EXPECT_EQ(count, 0u);
  EXPECT_EQ(queued, 0u);
}

namespace {
constexpr uint32_t archiveTime = 1800000000;
std::string syncedPath(const std::string& book = "/book.epub") {
  return "/.crosspoint/reading-events/" + localDataKey(book) + ".bin.synced";
}
ReadingEvent pageEvent(uint32_t page = 1) {
  ReadingEvent event;
  event.boot = 42;
  event.uptime = 123 + page;
  event.stat.startTime = archiveTime - 100;
  event.stat.durationSeconds = 17;
  event.stat.page = page;
  event.stat.totalPages = 10000;
  return event;
}
}  // namespace

TEST_F(BookOrbitDataStoreTest, ArchivesExactResolvedUploadAndKeepsConcurrentAppendPending) {
  ReadingQueue queue("/book.epub");
  auto event = pageEvent();
  event.stat.startTime = 0;
  ASSERT_TRUE(queue.append(&event, 1));
  auto later = pageEvent(2);
  ASSERT_TRUE(queue.append(&later, 1));
  event.stat.startTime = archiveTime - 100;
  ASSERT_TRUE(queue.acknowledgeSynced(&event, 1, archiveTime));
  const auto& bytes = fake::files.at(syncedPath())->bytes;
  ASSERT_EQ(bytes.size(), 28u);
  uint32_t timestamp;
  memcpy(&timestamp, bytes.data(), 4);
  EXPECT_EQ(timestamp, archiveTime);
  EXPECT_EQ(memcmp(bytes.data() + 4, &event, sizeof(event)), 0);
  ReadingEvent remaining;
  unsigned count;
  ASSERT_TRUE(queue.read(&remaining, 1, count));
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(remaining.stat.page, 2u);
}

TEST_F(BookOrbitDataStoreTest, ArchiveFailuresNeverRemoveQueuedEvents) {
  for (int failure = 0; failure < 3; ++failure) {
    fake::reset();
    ReadingQueue queue("/book.epub");
    auto event = pageEvent();
    ASSERT_TRUE(queue.append(&event, 1));
    if (failure == 0) fake::failWrite = 0;
    if (failure == 1) fake::failClosePath = syncedPath() + ".tmp";
    if (failure == 2) fake::failRename = 0;
    EXPECT_FALSE(queue.acknowledgeSynced(&event, 1, archiveTime));
    unsigned count;
    ASSERT_TRUE(queue.read(&event, 1, count));
    EXPECT_EQ(count, 1u);
    ASSERT_TRUE(queue.acknowledgeSynced(&event, 1, archiveTime));
    EXPECT_EQ(fake::files.at(syncedPath())->bytes.size(), 28u);
  }
}

TEST_F(BookOrbitDataStoreTest, RetryAfterQueueInstallFailureDoesNotDuplicateOrExtendArchive) {
  ReadingQueue queue("/book.epub");
  auto event = pageEvent();
  ASSERT_TRUE(queue.append(&event, 1));
  fake::failRename = 2;  // Archive install succeeds; queue staging install fails.
  ASSERT_FALSE(queue.acknowledgeSynced(&event, 1, archiveTime));
  unsigned count;
  ASSERT_TRUE(queue.read(&event, 1, count));
  ASSERT_EQ(count, 1u);
  ASSERT_TRUE(queue.acknowledgeSynced(&event, 1, archiveTime + 100));
  const auto& bytes = fake::files.at(syncedPath())->bytes;
  ASSERT_EQ(bytes.size(), 28u);
  uint32_t timestamp;
  memcpy(&timestamp, bytes.data(), 4);
  EXPECT_EQ(timestamp, archiveTime);
  ASSERT_TRUE(queue.read(&event, 1, count));
  EXPECT_EQ(count, 0u);
}

TEST_F(BookOrbitDataStoreTest, ExpiryUsesSyncTimeAndCleansInactiveBooksAtThirtyDays) {
  ReadingQueue queue("/old-book.epub");
  auto event = pageEvent();
  event.stat.startTime = archiveTime - 60 * 86400;  // Old reading synced today.
  ASSERT_TRUE(queue.append(&event, 1));
  ASSERT_TRUE(queue.acknowledgeSynced(&event, 1, archiveTime));
  const auto original = fake::files.at(syncedPath("/old-book.epub"))->bytes;
  ASSERT_TRUE(ReadingQueue::pruneSynced(0));
  ASSERT_TRUE(ReadingQueue::pruneSynced(archiveTime - 1));
  ASSERT_TRUE(ReadingQueue::pruneSynced(archiveTime + ReadingQueue::RETENTION_SECONDS - 1));
  EXPECT_EQ(fake::files.at(syncedPath("/old-book.epub"))->bytes, original);
  ASSERT_TRUE(ReadingQueue::pruneSynced(archiveTime + ReadingQueue::RETENTION_SECONDS));
  EXPECT_TRUE(fake::files.at(syncedPath("/old-book.epub"))->bytes.empty());
}

TEST_F(BookOrbitDataStoreTest, CleanupKeepsRecentAndPendingAndUndatedEvents) {
  ReadingQueue queue("/book.epub");
  auto event = pageEvent();
  ASSERT_TRUE(queue.append(&event, 1));
  ASSERT_TRUE(queue.acknowledgeSynced(&event, 1, archiveTime));
  event = pageEvent(2);
  ASSERT_TRUE(queue.append(&event, 1));
  ASSERT_TRUE(queue.acknowledgeSynced(&event, 1, archiveTime + 86400));
  event = pageEvent(3);
  ASSERT_TRUE(queue.append(&event, 1));
  ASSERT_TRUE(queue.preserveUndated(event));
  ASSERT_TRUE(ReadingQueue::pruneSynced(archiveTime + ReadingQueue::RETENTION_SECONDS));
  const auto& bytes = fake::files.at(syncedPath())->bytes;
  ASSERT_EQ(bytes.size(), 28u);
  ReadingEvent saved;
  memcpy(&saved, bytes.data() + 4, sizeof(saved));
  EXPECT_EQ(saved.stat.page, 2u);
  unsigned count;
  ASSERT_TRUE(queue.read(&saved, 1, count));
  EXPECT_EQ(count, 1u);
  EXPECT_EQ(saved.stat.page, 3u);
  EXPECT_EQ(fake::files.at("/.crosspoint/reading-events/" + localDataKey("/book.epub") + ".bin.undated")->bytes.size(),
            24u);
}

TEST_F(BookOrbitDataStoreTest, UnknownTimeOrWrongPrefixCannotAcknowledge) {
  ReadingQueue queue("/book.epub");
  auto event = pageEvent();
  ASSERT_TRUE(queue.append(&event, 1));
  EXPECT_FALSE(queue.acknowledgeSynced(&event, 1, 0));
  ++event.stat.page;
  EXPECT_FALSE(queue.acknowledgeSynced(&event, 1, archiveTime));
  unsigned count;
  ASSERT_TRUE(queue.read(&event, 1, count));
  EXPECT_EQ(count, 1u);
  EXPECT_FALSE(Storage.exists(syncedPath().c_str()));
}

TEST_F(BookOrbitDataStoreTest, FailedExpiryInstallPreservesArchiveForRetry) {
  ReadingQueue queue("/book.epub");
  auto event = pageEvent();
  ASSERT_TRUE(queue.append(&event, 1));
  ASSERT_TRUE(queue.acknowledgeSynced(&event, 1, archiveTime));
  const auto original = fake::files.at(syncedPath())->bytes;
  fake::failRename = 1;
  EXPECT_FALSE(ReadingQueue::pruneSynced(archiveTime + ReadingQueue::RETENTION_SECONDS));
  EXPECT_EQ(fake::files.at(syncedPath())->bytes, original);
  ASSERT_TRUE(ReadingQueue::pruneSynced(archiveTime + ReadingQueue::RETENTION_SECONDS));
  EXPECT_TRUE(fake::files.at(syncedPath())->bytes.empty());
}

TEST_F(BookOrbitDataStoreTest, CorruptArchiveNeverAcknowledgesMoreEvents) {
  ReadingQueue queue("/book.epub");
  auto event = pageEvent();
  ASSERT_TRUE(queue.append(&event, 1));
  fake::add(syncedPath(), "truncated");
  EXPECT_FALSE(queue.acknowledgeSynced(&event, 1, archiveTime));
  EXPECT_FALSE(ReadingQueue::pruneSynced(archiveTime + ReadingQueue::RETENTION_SECONDS));
  unsigned count;
  ASSERT_TRUE(queue.read(&event, 1, count));
  EXPECT_EQ(count, 1u);
}

TEST_F(BookOrbitDataStoreTest, CleanupRecoversInterruptedArchiveOfInactiveBook) {
  ReadingQueue queue("/book.epub");
  auto event = pageEvent();
  ASSERT_TRUE(queue.append(&event, 1));
  ASSERT_TRUE(queue.acknowledgeSynced(&event, 1, archiveTime));
  ASSERT_TRUE(Storage.rename(syncedPath().c_str(), (syncedPath() + ".bak").c_str()));
  ASSERT_TRUE(ReadingQueue::pruneSynced(archiveTime + ReadingQueue::RETENTION_SECONDS));
  ASSERT_TRUE(Storage.exists(syncedPath().c_str()));
  EXPECT_TRUE(fake::files.at(syncedPath())->bytes.empty());
}

TEST_F(BookOrbitDataStoreTest, CleanupRemovesStaleBackupEvenWhenLiveArchiveIsRecent) {
  ReadingQueue queue("/book.epub");
  auto event = pageEvent();
  ASSERT_TRUE(queue.append(&event, 1));
  ASSERT_TRUE(queue.acknowledgeSynced(&event, 1, archiveTime));
  const auto old = fake::files.at(syncedPath())->bytes;
  event = pageEvent(2);
  ASSERT_TRUE(queue.append(&event, 1));
  ASSERT_TRUE(queue.acknowledgeSynced(&event, 1, archiveTime + ReadingQueue::RETENTION_SECONDS));
  // Power loss after installing the new archive but before removing its backup.
  fake::add(syncedPath() + ".bak", std::string(old.begin(), old.end()));
  ASSERT_TRUE(ReadingQueue::pruneSynced(archiveTime + ReadingQueue::RETENTION_SECONDS));
  EXPECT_FALSE(Storage.exists((syncedPath() + ".bak").c_str()));
  EXPECT_EQ(fake::files.at(syncedPath())->bytes.size(), 28u);
}

TEST_F(BookOrbitDataStoreTest, FailedQueueCloseKeepsOriginalEventsForAppendAndAcknowledgement) {
  ReadingQueue queue("/book.epub");
  auto first = pageEvent(1), next = pageEvent(2);
  ASSERT_TRUE(queue.append(&first, 1));
  const auto staging = "/.crosspoint/reading-events/" + localDataKey("/book.epub") + ".bin.tmp";
  fake::failClosePath = staging;
  EXPECT_FALSE(queue.append(&next, 1));
  ReadingEvent events[2];
  unsigned count;
  ASSERT_TRUE(queue.read(events, 2, count));
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(events[0].stat.page, 1u);
  fake::failClosePath = staging;
  EXPECT_FALSE(queue.acknowledgeSynced(&first, 1, archiveTime));
  ASSERT_TRUE(queue.read(events, 2, count));
  EXPECT_EQ(count, 1u);
  ASSERT_TRUE(queue.acknowledgeSynced(&first, 1, archiveTime));
  ASSERT_TRUE(queue.read(events, 2, count));
  EXPECT_EQ(count, 0u);
  EXPECT_EQ(fake::files.at(syncedPath())->bytes.size(), 28u);
}

TEST_F(BookOrbitDataStoreTest, FailedAnnotationIndexCloseKeepsOriginalMembership) {
  AnnotationStore store("/annotations");
  ASSERT_TRUE(store.open());
  ASSERT_TRUE(store.put(entry()));
  auto next = entry();
  next.key = std::string(32, 'b');
  fake::failClosePath = "/annotations/index.tmp";
  EXPECT_FALSE(store.put(next));
  AnnotationStore reopened("/annotations");
  ASSERT_TRUE(reopened.open());
  EXPECT_EQ(reopened.count(), 1u);
  ASSERT_TRUE(reopened.put(next));
  EXPECT_EQ(reopened.count(), 2u);
}

TEST_F(BookOrbitDataStoreTest, UndatedCloseFailureCannotAcknowledgePersistence) {
  ReadingQueue queue("/book.epub");
  auto event = pageEvent();
  ASSERT_TRUE(queue.append(&event, 1));
  fake::failClosePath = "/.crosspoint/reading-events/" + localDataKey("/book.epub") + ".bin.undated";
  EXPECT_FALSE(queue.preserveUndated(event));
  unsigned count;
  ASSERT_TRUE(queue.read(&event, 1, count));
  EXPECT_EQ(count, 1u);
}
