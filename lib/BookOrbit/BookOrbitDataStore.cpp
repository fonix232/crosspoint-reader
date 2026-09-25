#include "BookOrbitDataStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <PersistableStore.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "BookOrbitPaths.h"
#include "BookOrbitStorage.h"

namespace bookorbit {
static_assert(sizeof(ReadingEvent) == 24, "reading event disk record must remain 24 bytes");
namespace {
constexpr char TAG[] = "BODATA";
class QueueLock {
 public:
  QueueLock() {
    if (mutex()) held = xSemaphoreTake(mutex(), portMAX_DELAY) == pdTRUE;
  }
  ~QueueLock() {
    if (held) xSemaphoreGive(mutex());
  }
  explicit operator bool() const { return held; }

 private:
  static SemaphoreHandle_t mutex() {
    static SemaphoreHandle_t value = xSemaphoreCreateMutex();
    return value;
  }
  bool held = false;
};
constexpr char LOCAL_ROOT[] = "/.crosspoint/reading-events";

using storage::recover;
using storage::replace;

struct SyncedEvent {
  uint32_t archivedAt;
  ReadingEvent event;
};
static_assert(sizeof(SyncedEvent) == 28, "synced event disk record must remain 28 bytes");
constexpr uint32_t MIN_ARCHIVE_TIME = 1704067200;  // 2024-01-01, same clock floor as sync.

bool expired(const SyncedEvent& record, uint32_t now) {
  return now >= record.archivedAt && now - record.archivedAt >= ReadingQueue::RETENTION_SECONDS;
}

// Stream the archive instead of loading a month's reading into RAM. Installing
// it before the queue rewrite makes a failed acknowledgement safely retryable.
bool archiveBatch(const std::string& path, const ReadingEvent* events, unsigned count, uint32_t now) {
  if (now < MIN_ARCHIVE_TIME || count > ReadingQueue::BATCH || !recover(path)) return false;
  const std::string staging = path + ".tmp";
  uint32_t found = 0;
  {
    HalFile input, output;
    if (Storage.exists(path.c_str()) &&
        (!Storage.openFileForRead(TAG, path, input) || input.fileSize() % sizeof(SyncedEvent)))
      return false;
    if (!Storage.openFileForWrite(TAG, staging, output)) return false;
    SyncedEvent record;
    while (input && input.position() < input.fileSize()) {
      if (input.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record) ||
          record.archivedAt < MIN_ARCHIVE_TIME)
        return false;
      if (expired(record, now)) continue;
      for (unsigned i = 0; i < count; ++i) {
        if (memcmp(&record.event, &events[i], sizeof(ReadingEvent)) == 0) found |= 1u << i;
      }
      if (output.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) != sizeof(record)) return false;
    }
    for (unsigned i = 0; i < count; ++i) {
      if (found & (1u << i)) continue;
      record = {now, events[i]};
      if (output.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) != sizeof(record)) return false;
    }
    output.flush();
    if (!output.close()) return false;
  }
  return replace(staging, path);
}

bool readAnnotation(const std::string& path, Annotation& out) {
  if (!recover(path)) return false;
  {
    HalFile file;
    if (!Storage.openFileForRead(TAG, path, file) || file.fileSize() > 24 * 1024) return false;
  }
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path.c_str(), doc)) return false;
  if (doc["schema"] != 1 || !doc["key"].is<const char*>()) return false;
  out = Annotation{};
  out.key = doc["key"] | "";
  out.datetime = doc["datetime"] | "";
  out.updated = doc["updated"] | "";
  out.pos = doc["pos"] | "";
  out.end = doc["end"] | "";
  out.text = doc["text"] | "";
  out.title = doc["title"] | "";
  out.note = doc["note"] | "";
  out.serverId = doc["serverId"] | 0u;
  out.version = doc["version"] | 0u;
  out.spine = doc["spine"] | 0u;
  out.offset = doc["offset"] | 0u;
  out.endOffset = doc["endOffset"] | 0u;
  out.percentage = doc["percentage"] | 0.0f;
  out.mapped = doc["mapped"] | false;
  out.deleted = doc["deleted"] | false;
  out.pendingAck = doc["pendingAck"] | false;
  out.dirty = doc["dirty"] | true;
  return validDocumentHash(out.key) && out.datetime.size() == 19 && out.pos.size() <= 4000 && out.end.size() <= 4000 &&
         out.text.size() <= 4096;
}

bool copyBytes(HalFile& input, HalFile& output, size_t bytes) {
  uint8_t buffer[192];
  while (bytes > 0) {
    const size_t chunk = bytes < sizeof(buffer) ? bytes : sizeof(buffer);
    if (input.read(buffer, chunk) != static_cast<int>(chunk) || output.write(buffer, chunk) != chunk) return false;
    bytes -= chunk;
  }
  return true;
}
}  // namespace

std::string localDataKey(const std::string& bookPath) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : bookPath) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  char value[17];
  snprintf(value, sizeof(value), "%016llx", static_cast<unsigned long long>(hash));
  return value;
}

std::string annotationDirectory(const std::string& account, const std::string& bookPath, AnnotationKind kind) {
  return accountDir(account) + "/annotations/" + localDataKey(bookPath) +
         (kind == AnnotationKind::Bookmark ? "-bookmarks" : "-highlights");
}

bool annotationIdentity(Annotation& entry, const uint32_t epoch) {
  if (entry.pos.empty()) return false;
  if (entry.datetime.empty()) {
    // Identity is minted once and persisted, never re-dated after a clock correction.
    const time_t when = epoch >= 946684800 ? epoch : 946684800;
    struct tm utc;
    gmtime_r(&when, &utc);
    char datetime[20];
    strftime(datetime, sizeof(datetime), "%Y-%m-%d %H:%M:%S", &utc);
    entry.datetime = datetime;
  }
  if (entry.datetime.size() != 19) return false;
  MD5Builder md5;
  md5.begin();
  const std::string identity = entry.datetime + "|" + entry.pos;
  md5.add(identity.c_str());
  md5.calculate();
  entry.key = md5.toString().c_str();
  return validDocumentHash(entry.key);
}

std::string AnnotationStore::pathFor(const std::string& key) const { return directory + "/" + key + ".json"; }

bool AnnotationStore::open() {
  total = 0;
  history = Storage.exists((directory + "/history").c_str());
  const std::string path = directory + "/index";
  if (!recover(path)) return false;
  if (!Storage.exists(path.c_str())) {
    history = false;
    return true;
  }
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file) || file.fileSize() % 32 != 0) return false;
  total = file.fileSize() / 32;
  return total <= 8192;
}

bool AnnotationStore::read(unsigned ordinal, Annotation& out) const {
  if (ordinal >= total) return false;
  HalFile file;
  if (!Storage.openFileForRead(TAG, directory + "/index", file) || !file.seek(ordinal * 32)) return false;
  char key[33] = {};
  if (file.read(key, 32) != 32 || !validDocumentHash(key)) return false;
  return readAnnotation(pathFor(key), out);
}

bool AnnotationStore::find(const std::string& key, Annotation& out) const {
  return validDocumentHash(key) && readAnnotation(pathFor(key), out);
}

bool AnnotationStore::findServer(uint32_t id, Annotation& out) const {
  for (unsigned i = 0; i < total; ++i)
    if (read(i, out) && out.serverId == id) return true;
  return false;
}

bool AnnotationStore::put(const Annotation& entry) {
  if (!validDocumentHash(entry.key) || entry.datetime.size() != 19 || entry.text.size() > 4096 ||
      entry.pos.size() > 4000 || entry.end.size() > 4000)
    return false;
  if (!Storage.exists(directory.c_str()) && !Storage.mkdir(directory.c_str(), true)) return false;
  // Membership is determined by the index, not a possibly orphaned record from
  // an interrupted index update.
  bool known = false;
  {
    HalFile index;
    if (Storage.exists((directory + "/index").c_str())) {
      if (!Storage.openFileForRead(TAG, directory + "/index", index)) return false;
      char key[32];
      while (index.read(key, sizeof(key)) == sizeof(key)) {
        if (memcmp(key, entry.key.data(), 32) == 0) {
          known = true;
          break;
        }
      }
    }
  }
  if (!known && total >= 8192) return false;
  JsonDocument doc;
  doc["schema"] = 1;
  doc["key"] = entry.key;
  doc["datetime"] = entry.datetime;
  doc["updated"] = entry.updated;
  doc["pos"] = entry.pos;
  doc["end"] = entry.end;
  doc["text"] = entry.text;
  doc["title"] = entry.title;
  doc["note"] = entry.note;
  doc["serverId"] = entry.serverId;
  doc["version"] = entry.version;
  doc["spine"] = entry.spine;
  doc["offset"] = entry.offset;
  doc["endOffset"] = entry.endOffset;
  doc["percentage"] = entry.percentage;
  doc["mapped"] = entry.mapped;
  doc["deleted"] = entry.deleted;
  doc["pendingAck"] = entry.pendingAck;
  doc["dirty"] = entry.dirty;
  if (doc.overflowed()) return false;
  const std::string path = pathFor(entry.key);
  const std::string temp = path + ".tmp";
  if (!PersistableStoreBase::writeDocToFile(temp.c_str(), doc) || !replace(temp, path)) return false;
  if (known) return true;
  const std::string indexPath = directory + "/index";
  const std::string staging = indexPath + ".tmp";
  {
    HalFile output;
    if (!Storage.openFileForWrite(TAG, staging, output)) return false;
    if (total > 0) {
      HalFile input;
      if (!Storage.openFileForRead(TAG, indexPath, input) || !copyBytes(input, output, total * 32)) return false;
    }
    if (output.write(reinterpret_cast<const uint8_t*>(entry.key.data()), 32) != 32) return false;
    output.flush();
    if (!output.close()) return false;
  }
  if (!replace(staging, indexPath)) return false;
  ++total;
  return true;
}

bool AnnotationStore::markHistory() {
  if (history) return true;
  // Keep a separate guard so losing an index/history file cannot turn the
  // rebuilt subset into an authoritative deletion inventory on the next sync.
  const std::string guard = directory + ".guard";
  if (Storage.exists(guard.c_str())) return true;
  if (!Storage.exists(directory.c_str()) && !Storage.mkdir(directory.c_str(), true)) return false;
  {
    HalFile index;
    if (total == 0 && !Storage.openFileForWrite(TAG, directory + "/index", index)) return false;
  }
  {
    HalFile file;
    if (!Storage.openFileForWrite(TAG, guard, file)) return false;
  }
  HalFile file;
  if (!Storage.openFileForWrite(TAG, directory + "/history", file)) return false;
  history = true;
  return true;
}

bool AnnotationStore::eraseLocal(const std::string& key) {
  Annotation entry;
  if (!find(key, entry)) return false;
  entry.deleted = true;
  entry.pendingAck = false;
  entry.dirty = false;
  return put(entry);
}

ReadingQueue::ReadingQueue(std::string bookPath)
    : path(std::string(LOCAL_ROOT) + "/" + localDataKey(bookPath) + ".bin") {}

bool ReadingQueue::append(const ReadingEvent* events, unsigned count) {
  QueueLock lock;
  if (!lock) return false;
  if (count == 0) return true;
  Storage.mkdir(LOCAL_ROOT, true);
  if (!recover(path)) return false;
  const std::string staging = path + ".tmp";
  {
    HalFile output, input;
    size_t previous = 0;
    if (Storage.exists(path.c_str())) {
      if (!Storage.openFileForRead(TAG, path, input)) return false;
      previous = input.fileSize();
      if (previous % sizeof(ReadingEvent) != 0) return false;
    }
    if (previous / sizeof(ReadingEvent) + count > MAX_EVENTS || !Storage.openFileForWrite(TAG, staging, output))
      return false;
    if (previous && !copyBytes(input, output, previous)) return false;
    const size_t bytes = count * sizeof(ReadingEvent);
    if (output.write(reinterpret_cast<const uint8_t*>(events), bytes) != bytes) return false;
    output.flush();
    if (!output.close()) return false;
  }
  return replace(staging, path);
}

bool ReadingQueue::read(ReadingEvent* events, unsigned capacity, unsigned& count, unsigned* queued) const {
  QueueLock lock;
  if (!lock) return false;
  count = 0;
  if (queued) *queued = 0;
  if (!recover(path)) return false;
  if (!Storage.exists(path.c_str())) return true;
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file) || file.fileSize() % sizeof(ReadingEvent) != 0) return false;
  const unsigned total = file.fileSize() / sizeof(ReadingEvent);
  if (queued) *queued = total;
  count = total < capacity ? total : capacity;
  const size_t bytes = count * sizeof(ReadingEvent);
  return file.read(reinterpret_cast<uint8_t*>(events), bytes) == static_cast<int>(bytes);
}

bool ReadingQueue::preserveUndated(const ReadingEvent& event) {
  QueueLock lock;
  if (!lock) return false;
  // A boot that never acquired a clock cannot be dated after its uptime is
  // lost. Preserve its event separately so it cannot block later dated pages.
  HalFile file = Storage.open((path + ".undated").c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!file || file.fileSize() + sizeof(event) > MAX_EVENTS * sizeof(event)) return false;
  const bool ok = file.write(reinterpret_cast<const uint8_t*>(&event), sizeof(event)) == sizeof(event);
  file.flush();
  const bool closed = file.close();
  return ok && closed;
}

bool ReadingQueue::acknowledge(unsigned count) {
  QueueLock lock;
  if (!lock) return false;
  return acknowledgeLocked(count);
}

bool ReadingQueue::acknowledgeSynced(const ReadingEvent* events, unsigned count, uint32_t now) {
  QueueLock lock;
  if (!lock || !events || !count || count > BATCH || !recover(path)) return false;
  // Only archive an actual queue prefix. startTime may have been resolved from
  // a clock anchor since it was queued; every other field must still match.
  {
    HalFile input;
    if (!Storage.openFileForRead(TAG, path, input) || input.fileSize() % sizeof(ReadingEvent)) return false;
    ReadingEvent original;
    for (unsigned i = 0; i < count; ++i) {
      if (input.read(reinterpret_cast<uint8_t*>(&original), sizeof(original)) != sizeof(original)) return false;
      original.stat.startTime = events[i].stat.startTime;
      if (memcmp(&original, &events[i], sizeof(original)) != 0) return false;
    }
  }
  if (!archiveBatch(path + ".synced", events, count, now)) return false;
  return acknowledgeLocked(count);
}

bool ReadingQueue::pruneSynced(uint32_t now) {
  if (now < MIN_ARCHIVE_TIME) return true;  // An unknown clock must never erase history.
  QueueLock lock;
  if (!lock) return false;
  if (!Storage.exists(LOCAL_ROOT)) return true;
  HalFile directory = Storage.open(LOCAL_ROOT);
  if (!directory || !directory.isDirectory()) return false;
  bool ok = true;
  char name[64];
  for (HalFile file = directory.openNextFile(); file; file = directory.openNextFile()) {
    file.getName(name, sizeof(name));
    const size_t length = strlen(name);
    if (file.isDirectory() || (length != 27 && length != 31)) continue;
    if (length == 31 && strcmp(name + 16, ".bin.synced.bak") == 0) {
      // Recover an archive interrupted between the two install renames, even
      // if its book is no longer selected for syncing.
      file.close();
      name[27] = '\0';
      const std::string live = std::string(LOCAL_ROOT) + "/" + name;
      if (!recover(live) || !Storage.openFileForRead(TAG, live, file)) {
        ok = false;
        continue;
      }
    } else if (length != 27 || strcmp(name + 16, ".bin.synced") != 0) {
      continue;
    }
    // Reinstall a recovered/leftover backup even if the live records are young,
    // so an older backup cannot outlive the retention window indefinitely.
    bool needsPrune = length == 31;
    SyncedEvent record;
    if (file.fileSize() % sizeof(record)) {
      ok = false;
      continue;
    }
    while (file.position() < file.fileSize()) {
      if (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record) ||
          record.archivedAt < MIN_ARCHIVE_TIME) {
        ok = false;
        break;
      }
      if (expired(record, now)) {
        needsPrune = true;
        break;
      }
    }
    file.close();
    if (needsPrune && !archiveBatch(std::string(LOCAL_ROOT) + "/" + name, nullptr, 0, now)) ok = false;
  }
  return ok;
}

bool ReadingQueue::acknowledgeLocked(unsigned count) {
  if (!recover(path)) return false;
  HalFile input;
  if (!Storage.openFileForRead(TAG, path, input)) return false;
  const size_t skip = count * sizeof(ReadingEvent);
  if (skip > input.fileSize()) return false;
  const std::string staging = path + ".tmp";
  {
    HalFile output;
    if (!input.seek(skip) || !Storage.openFileForWrite(TAG, staging, output) ||
        !copyBytes(input, output, input.fileSize() - skip))
      return false;
    output.flush();
    if (!output.close()) return false;
  }
  input.close();
  return replace(staging, path);
}

}  // namespace bookorbit
