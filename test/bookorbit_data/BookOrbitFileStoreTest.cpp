#include <HalStorage.h>
#include <gtest/gtest.h>

#include "BookOrbitFileStore.h"
#include "BookOrbitPaths.h"

using namespace bookorbit;

class BookOrbitFileStoreTest : public testing::Test {
 protected:
  void SetUp() override { fake::reset(); }
  ManagedFile book(uint32_t id) {
    ManagedFile value;
    value.bookId = id;
    value.fileId = id + 100;
    value.flags = FILE_FLAG_COMPLETE;
    return value;
  }
};

TEST_F(BookOrbitFileStoreTest, FailedWriteCloseOrInstallPreservesEveryRecord) {
  for (int failure = 0; failure < 3; ++failure) {
    fake::reset();
    FileStore files;
    ASSERT_TRUE(files.open("account"));
    ASSERT_TRUE(files.upsert(book(1)));
    ASSERT_TRUE(files.upsert(book(2)));
    const auto original = fake::files[filesIndexPath("account")]->bytes;
    if (failure == 0) fake::failWrite = 0;
    if (failure == 1) fake::failClosePath = filesIndexStagingPath("account");
    if (failure == 2) fake::failRename = 1;
    EXPECT_FALSE(files.upsert(book(3)));
    EXPECT_TRUE(fake::failureTriggered);
    EXPECT_EQ(fake::files[filesIndexPath("account")]->bytes, original);
    ASSERT_TRUE(files.open("account"));
    EXPECT_EQ(files.count(), 2);
    EXPECT_TRUE(files.isComplete(1));
    EXPECT_TRUE(files.isComplete(2));
  }
}

TEST_F(BookOrbitFileStoreTest, MissingLiveIndexCannotDiscardResidentEntries) {
  FileStore files;
  ASSERT_TRUE(files.open("account"));
  ASSERT_TRUE(files.upsert(book(1)));
  ASSERT_TRUE(Storage.remove(filesIndexPath("account").c_str()));
  EXPECT_FALSE(files.upsert(book(2)));
  EXPECT_FALSE(Storage.exists(filesIndexPath("account").c_str()));
}

TEST_F(BookOrbitFileStoreTest, InterruptedInstallRecoversBackupAndRejectsPartialRecord) {
  FileStore files;
  ASSERT_TRUE(files.open("account"));
  ASSERT_TRUE(files.upsert(book(1)));
  const auto path = filesIndexPath("account");
  ASSERT_TRUE(Storage.rename(path.c_str(), (path + ".bak").c_str()));
  ASSERT_TRUE(files.open("account"));
  EXPECT_TRUE(files.isComplete(1));
  fake::files[path]->bytes.push_back(0);
  EXPECT_FALSE(files.open("account"));
  EXPECT_FALSE(files.isOpen());
}

TEST_F(BookOrbitFileStoreTest, FailedReloadCannotLeaveWritableEmptyIndex) {
  FileStore files;
  ASSERT_TRUE(files.open("account"));
  ASSERT_TRUE(files.upsert(book(1)));
  fake::failRead = 1;  // Copy succeeds; rereading the installed index fails.
  EXPECT_FALSE(files.upsert(book(2)));
  EXPECT_FALSE(files.isOpen());
  EXPECT_FALSE(files.upsert(book(3)));
  ASSERT_TRUE(files.open("account"));
  EXPECT_EQ(files.count(), 2);
}

TEST_F(BookOrbitFileStoreTest, ManagedPathRequiresDirectoryBoundary) {
  EXPECT_TRUE(isManagedPath("/.crosspoint/bookorbit/account/files/1-2.epub"));
  EXPECT_FALSE(isManagedPath("/.crosspoint/bookorbit-other/files/1-2.epub"));
  EXPECT_FALSE(isManagedPath("/.crosspoint/bookorbit"));
}
