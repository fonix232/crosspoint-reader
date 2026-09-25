#include "BookmarkFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include "BookmarkUtil.h"

bool BookmarkFile::load(const std::string& bookPath, std::vector<BookmarkEntry>& bookmarks) {
  bookmarks.clear();

  // Read/write go through PersistableStoreBase so the JSON parser and
  // serializer stay instantiated once, in PersistableStore.cpp.
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  const std::string backup = path + ".bak";
  if (!Storage.exists(path.c_str()) && Storage.exists(backup.c_str()) && !Storage.rename(backup.c_str(), path.c_str()))
    return false;
  {
    HalFile file;
    if (!Storage.openFileForRead("BKM", path, file) || file.fileSize() > 256 * 1024) return false;
  }
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path.c_str(), doc)) {
    return false;
  }

  JsonArray arr = doc["bookmarks"].as<JsonArray>();
  if (arr.isNull() || arr.size() > 500) return false;
  bookmarks.reserve(arr.size());
  for (JsonObject obj : arr) {
    bookmarks.emplace_back();
    auto& bookmark = bookmarks.back();
    bookmark.syncDatetime = obj["syncDatetime"] | "";
    bookmark.xpath = obj["xpath"] | "";
    bookmark.percentage = obj["percentage"] | static_cast<float>(0);
    bookmark.summary = obj["summary"] | "";
    bookmark.name = obj["name"] | "";
    if (bookmark.name.size() > BookmarkEntry::MAX_NAME_LENGTH) {
      bookmark.name.resize(BookmarkEntry::MAX_NAME_LENGTH);
    }
    bookmark.computedSpineIndex = obj["si"] | static_cast<uint16_t>(0);
    bookmark.computedChapterPageCount = obj["pc"] | static_cast<uint16_t>(0);
    bookmark.computedChapterProgress = obj["pp"] | static_cast<uint16_t>(0);
    if (!obj["vo"].isNull()) {
      bookmark.visibleTextOffset = obj["vo"] | static_cast<uint32_t>(0);
      bookmark.hasVisibleTextOffset = true;
    }
  }

  LOG_DBG("BKM", "Loaded %zu bookmarks from file", bookmarks.size());
  return true;
}

bool BookmarkFile::save(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks) {
  for (const auto& bookmark : bookmarks) {
    if (bookmark.name.size() > BookmarkEntry::MAX_NAME_LENGTH) {
      LOG_ERR("BKM", "Bookmark name exceeds %zu bytes", BookmarkEntry::MAX_NAME_LENGTH);
      return false;
    }
  }

  JsonDocument doc;
  JsonArray arr = doc["bookmarks"].to<JsonArray>();
  LOG_DBG("BKM", "Saving %zu bookmarks to file", bookmarks.size());
  for (const auto& bookmark : bookmarks) {
    JsonObject obj = arr.add<JsonObject>();
    if (!bookmark.syncDatetime.empty()) obj["syncDatetime"] = bookmark.syncDatetime;
    obj["xpath"] = bookmark.xpath;
    obj["percentage"] = bookmark.percentage;
    obj["summary"] = bookmark.summary;
    if (!bookmark.name.empty()) {
      obj["name"] = bookmark.name;
    }
    obj["si"] = bookmark.computedSpineIndex;
    obj["pc"] = bookmark.computedChapterPageCount;
    obj["pp"] = bookmark.computedChapterProgress;
    if (bookmark.hasVisibleTextOffset) {
      obj["vo"] = bookmark.visibleTextOffset;
    }
  }

  // writeDocToFile ensures /.crosspoint; the bookmarks subdirectory is ours.
  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  const std::string staging = path + ".tmp", backup = path + ".bak";
  if (!Storage.exists(path.c_str()) && Storage.exists(backup.c_str()) && !Storage.rename(backup.c_str(), path.c_str()))
    return false;
  if (!PersistableStoreBase::writeDocToFile(staging.c_str(), doc)) return false;
  if (Storage.exists(backup.c_str()) && !Storage.remove(backup.c_str())) return false;
  const bool previous = Storage.exists(path.c_str());
  if (previous && !Storage.rename(path.c_str(), backup.c_str())) return false;
  if (!Storage.rename(staging.c_str(), path.c_str())) {
    if (previous) Storage.rename(backup.c_str(), path.c_str());
    return false;
  }
  if (previous) Storage.remove(backup.c_str());
  return true;
}
