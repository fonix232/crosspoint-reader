#include "BookOrbitDownloadQueue.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <PersistableStore.h>

#include <algorithm>

#include "BookOrbitPaths.h"

namespace bookorbit {

std::string queuePath(const std::string& accountKey) { return accountDir(accountKey) + "/queue.json"; }

bool DownloadQueue::load(const std::string& accountKey) {
  account_ = accountKey;
  items_.clear();
  JsonDocument doc;
  const std::string path = queuePath(accountKey);
  if (!Storage.exists(path.c_str()) || !PersistableStoreBase::readDocFromFile(path.c_str(), doc)) return false;
  for (const JsonVariantConst entry : doc["items"].as<JsonArrayConst>()) {
    if (items_.size() >= MAX_ITEMS) break;
    QueueItem item;
    item.bookId = entry["id"] | 0u;
    item.state = (entry["state"] | 0) == static_cast<int>(QueueState::Failed) ? QueueState::Failed : QueueState::Queued;
    item.error = entry["error"] | 0;
    if (item.bookId != 0 && !contains(item.bookId)) items_.push_back(item);
  }
  return true;
}

bool DownloadQueue::save() const {
  if (account_.empty()) return false;
  Storage.mkdir(accountDir(account_).c_str());
  const std::string path = queuePath(account_);
  if (items_.empty()) {
    Storage.remove(path.c_str());
    return true;
  }
  JsonDocument doc;
  JsonArray array = doc["items"].to<JsonArray>();
  for (const QueueItem& item : items_) {
    JsonObject entry = array.add<JsonObject>();
    entry["id"] = item.bookId;
    entry["state"] = static_cast<int>(item.state);
    if (item.error != 0) entry["error"] = item.error;
  }
  return PersistableStoreBase::writeDocToFile(path.c_str(), doc);
}

bool DownloadQueue::contains(const uint32_t bookId) const { return find(bookId) != nullptr; }

const QueueItem* DownloadQueue::find(const uint32_t bookId) const {
  for (const QueueItem& item : items_) {
    if (item.bookId == bookId) return &item;
  }
  return nullptr;
}

size_t DownloadQueue::queuedCount() const {
  return static_cast<size_t>(std::count_if(items_.begin(), items_.end(),
                                           [](const QueueItem& item) { return item.state == QueueState::Queued; }));
}

bool DownloadQueue::nextQueued(uint32_t& bookId) const {
  for (const QueueItem& item : items_) {
    if (item.state == QueueState::Queued) {
      bookId = item.bookId;
      return true;
    }
  }
  return false;
}

bool DownloadQueue::add(const uint32_t bookId) {
  if (bookId == 0) return false;
  for (QueueItem& item : items_) {
    if (item.bookId == bookId) {
      item.state = QueueState::Queued;
      item.error = 0;
      return true;
    }
  }
  if (items_.size() >= MAX_ITEMS) return false;
  QueueItem item;
  item.bookId = bookId;
  items_.push_back(item);
  return true;
}

bool DownloadQueue::remove(const uint32_t bookId) {
  const auto it =
      std::find_if(items_.begin(), items_.end(), [bookId](const QueueItem& item) { return item.bookId == bookId; });
  if (it == items_.end()) return false;
  items_.erase(it);
  return true;
}

bool DownloadQueue::markFailed(const uint32_t bookId, const uint8_t error) {
  for (QueueItem& item : items_) {
    if (item.bookId == bookId) {
      item.state = QueueState::Failed;
      item.error = error;
      return true;
    }
  }
  return false;
}

}  // namespace bookorbit
