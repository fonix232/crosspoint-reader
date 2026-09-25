#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bookorbit {

// The account's download queue: books the user asked for that are not on the
// card yet. Kept in `<account>/queue.json` so a queue survives a reboot or a
// sleep mid-transfer; the background agent drains it (and resumes it after
// boot), boards without an agent drain it with a foreground session. An
// entry is Queued until its download finishes (then it is removed) or fails
// (then it stays, marked, for a retry or a removal from the queue screen).
enum class QueueState : uint8_t { Queued = 0, Failed = 2 };

struct QueueItem {
  uint32_t bookId = 0;
  QueueState state = QueueState::Queued;
  uint8_t error = 0;  // ClientError of the failure
};

class DownloadQueue {
 public:
  static constexpr size_t MAX_ITEMS = 64;

  bool load(const std::string& accountKey);
  bool save() const;

  const std::vector<QueueItem>& items() const { return items_; }
  bool contains(uint32_t bookId) const;
  const QueueItem* find(uint32_t bookId) const;
  size_t queuedCount() const;
  // First item still to download, false when none.
  bool nextQueued(uint32_t& bookId) const;

  // Adds (or re-queues a failed) book; false when the queue is full.
  bool add(uint32_t bookId);
  bool remove(uint32_t bookId);
  bool markFailed(uint32_t bookId, uint8_t error);

 private:
  std::string account_;
  std::vector<QueueItem> items_;
};

std::string queuePath(const std::string& accountKey);

}  // namespace bookorbit
