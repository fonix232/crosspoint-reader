#include "BookOrbitFileStore.h"

#include <Logging.h>
#include <Memory.h>
#include <PersistableStore.h>

#include <cstring>

#include "BookOrbitPaths.h"
#include "BookOrbitStorage.h"

namespace bookorbit {

namespace {
constexpr char TAG[] = "BOFILE";

void put32(uint8_t* out, const uint32_t v) {
  out[0] = static_cast<uint8_t>(v);
  out[1] = static_cast<uint8_t>(v >> 8);
  out[2] = static_cast<uint8_t>(v >> 16);
  out[3] = static_cast<uint8_t>(v >> 24);
}
void put16(uint8_t* out, const uint16_t v) {
  out[0] = static_cast<uint8_t>(v);
  out[1] = static_cast<uint8_t>(v >> 8);
}
uint32_t get32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}
uint16_t get16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | (in[1] << 8)); }

bool readRecordAt(HalFile& file, const uint16_t index, ManagedFile& out) {
  uint8_t raw[FILE_RECORD_BYTES];
  if (!file.seekSet(static_cast<size_t>(index) * FILE_RECORD_BYTES)) return false;
  if (file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return false;
  decodeManagedFile(raw, out);
  return true;
}
}  // namespace

void encodeManagedFile(const ManagedFile& in, uint8_t out[FILE_RECORD_BYTES]) {
  memset(out, 0, FILE_RECORD_BYTES);
  put32(out, in.bookId);
  put32(out + 4, in.fileId);
  put32(out + 8, in.sizeBytes);
  put32(out + 12, in.downloadedAt);
  out[16] = in.flags;
  put16(out + 18, in.ackLocalSpine);
  put32(out + 20, in.ackLocalKey);
  put32(out + 24, in.ackRemoteTs);
  put32(out + 28, in.ackRemoteHash);
  memcpy(out + 32, in.contentVersion, FILE_VERSION_BYTES);
  memcpy(out + 64, in.fileHash, FILE_HASH_BYTES);
}

void decodeManagedFile(const uint8_t in[FILE_RECORD_BYTES], ManagedFile& out) {
  out = ManagedFile{};
  out.bookId = get32(in);
  out.fileId = get32(in + 4);
  out.sizeBytes = get32(in + 8);
  out.downloadedAt = get32(in + 12);
  out.flags = in[16];
  out.ackLocalSpine = get16(in + 18);
  out.ackLocalKey = get32(in + 20);
  out.ackRemoteTs = get32(in + 24);
  out.ackRemoteHash = get32(in + 28);
  memcpy(out.contentVersion, in + 32, FILE_VERSION_BYTES);
  out.contentVersion[FILE_VERSION_BYTES] = '\0';
  memcpy(out.fileHash, in + 64, FILE_HASH_BYTES);
  out.fileHash[FILE_HASH_BYTES] = '\0';
}

bool FileStore::open(const std::string& accountKey) {
  close();
  account = accountKey;
  opened = true;
  if (!loadIds()) {
    opened = false;
    return false;
  }
  return true;
}

void FileStore::close() {
  opened = false;
  total = 0;
  ids.reset();
  flags.reset();
}

bool FileStore::loadIds() {
  ids.reset();
  flags.reset();
  total = 0;
  const std::string path = filesIndexPath(account);
  if (!storage::recover(path)) return false;
  if (!Storage.exists(path.c_str())) return true;  // no downloads yet
  HalFile file;
  if (!Storage.openFileForRead(TAG, path.c_str(), file)) return false;
  const size_t size = file.fileSize();
  if (size % FILE_RECORD_BYTES != 0) return false;
  const size_t records = size / FILE_RECORD_BYTES;
  if (records == 0) return true;
  if (records > FILE_STORE_MAX) {
    LOG_ERR(TAG, "files index too large (%u records)", static_cast<unsigned>(records));
    return false;
  }
  auto array = makeUniqueNoThrow<uint32_t[]>(records);
  auto flagArray = makeUniqueNoThrow<uint8_t[]>(records);
  if (!array || !flagArray) {
    LOG_ERR(TAG, "OOM: %u-byte id table", static_cast<unsigned>(records * 5));
    return false;
  }
  uint8_t raw[FILE_RECORD_BYTES];
  for (size_t i = 0; i < records; i++) {
    if (!file.seekSet(i * FILE_RECORD_BYTES) || file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) {
      return false;
    }
    array[i] = get32(raw);
    flagArray[i] = raw[16];
  }
  ids = std::move(array);
  flags = std::move(flagArray);
  total = static_cast<uint16_t>(records);
  return true;
}

int FileStore::indexOfBook(const uint32_t bookId) const {
  if (!opened || !ids) return -1;
  for (uint16_t i = 0; i < total; i++) {
    if (ids[i] == bookId) return i;
  }
  return -1;
}

bool FileStore::read(const uint16_t index, ManagedFile& out) const {
  if (!opened || index >= total) return false;
  HalFile file;
  if (!Storage.openFileForRead(TAG, filesIndexPath(account).c_str(), file)) return false;
  return readRecordAt(file, index, out);
}

bool FileStore::find(const uint32_t bookId, ManagedFile& out) const {
  const int index = indexOfBook(bookId);
  return index >= 0 && read(static_cast<uint16_t>(index), out);
}

bool FileStore::isComplete(const uint32_t bookId) const {
  const int index = indexOfBook(bookId);
  return index >= 0 && flags && (flags[index] & FILE_FLAG_COMPLETE) != 0;
}

bool FileStore::hasConflict(const uint32_t bookId) const {
  const int index = indexOfBook(bookId);
  return index >= 0 && flags && (flags[index] & FILE_FLAG_CONFLICT) != 0;
}

std::string FileStore::pathFor(const ManagedFile& file) const {
  if (file.flags & FILE_FLAG_SIDELOAD) {
    JsonDocument doc;
    const std::string mapping = accountDir(account) + "/sideload-" + std::to_string(file.bookId) + ".json";
    if (!PersistableStoreBase::readDocFromFile(mapping.c_str(), doc)) return {};
    const std::string path = doc["path"] | "";
    if (path.empty() || path.front() != '/' || path.find("..") != std::string::npos || isManagedPath(path)) return {};
    return path;
  }
  return managedFilePath(account, file.bookId, file.fileId);
}

// Copies the live index to the staging path with one record replaced,
// appended, or dropped, then swaps it in. Writes are rare (a download, a sync
// acknowledgement), so the full copy is acceptable for the crash safety it buys.
bool FileStore::rewrite(const ManagedFile* replacement, const uint32_t removeBookId) {
  if (!opened) return false;
  const std::string live = filesIndexPath(account);
  const std::string tmp = filesIndexStagingPath(account);
  Storage.mkdir(accountDir(account).c_str());
  Storage.remove(tmp.c_str());
  {
    HalFile out;
    if (!Storage.openFileForWrite(TAG, tmp.c_str(), out)) return false;
    HalFile in;
    const bool haveLive = Storage.openFileForRead(TAG, live.c_str(), in);
    if (!haveLive && (total != 0 || Storage.exists(live.c_str()))) return false;
    bool replaced = false;
    uint16_t kept = 0;
    uint8_t raw[FILE_RECORD_BYTES];
    for (uint16_t i = 0; haveLive && i < total; i++) {
      if (!in.seekSet(static_cast<size_t>(i) * FILE_RECORD_BYTES) ||
          in.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) {
        return false;
      }
      const uint32_t bookId = get32(raw);
      if (bookId == removeBookId) continue;
      if (replacement && bookId == replacement->bookId) {
        encodeManagedFile(*replacement, raw);
        replaced = true;
      }
      if (out.write(raw, sizeof(raw)) != sizeof(raw)) return false;
      kept++;
    }
    if (replacement && !replaced) {
      if (kept >= FILE_STORE_MAX) {
        LOG_ERR(TAG, "files index full (%u)", FILE_STORE_MAX);
        return false;
      }
      encodeManagedFile(*replacement, raw);
      if (out.write(raw, sizeof(raw)) != sizeof(raw)) return false;
    }
    out.flush();
    if (!out.close()) return false;
  }
  if (!storage::replace(tmp, live)) {
    LOG_ERR(TAG, "cannot install files index");
    return false;
  }
  if (loadIds()) return true;
  close();
  return false;
}

bool FileStore::attachSideload(ManagedFile file, const std::string& path) {
  if (path.empty() || path.front() != '/' || path.find("..") != std::string::npos || isManagedPath(path)) return false;
  JsonDocument doc;
  doc["path"] = path;
  const std::string mapping = accountDir(account) + "/sideload-" + std::to_string(file.bookId) + ".json";
  if (!PersistableStoreBase::writeDocToFile(mapping.c_str(), doc)) return false;
  file.flags |= FILE_FLAG_SIDELOAD | FILE_FLAG_COMPLETE;
  return upsert(file);
}

bool FileStore::upsert(const ManagedFile& file) { return rewrite(&file, 0); }

bool FileStore::remove(const uint32_t bookId) { return rewrite(nullptr, bookId); }

bool FileStore::markRemoved(const uint32_t bookId) {
  ManagedFile file;
  if (!find(bookId, file)) return true;
  if (file.flags & FILE_FLAG_SIDELOAD) {
    file.flags &= static_cast<uint8_t>(~FILE_FLAG_COMPLETE);
    return upsert(file);  // Detach only, and remember the user's choice.
  }
  const std::string path = pathFor(file);
  if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
    LOG_ERR(TAG, "cannot delete %s", path.c_str());
    return false;
  }
  file.flags &= static_cast<uint8_t>(~FILE_FLAG_COMPLETE);
  return upsert(file);
}

bool FileStore::reconcile() {
  if (!opened) return false;
  // Orphaned staging files: a transfer interrupted by power loss. They are
  // retried from the beginning, so the partial bytes have no value.
  const std::string transfers = transfersDir(account);
  HalFile dir = Storage.open(transfers.c_str());
  if (dir && dir.isDirectory()) {
    char name[64];
    for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      if (entry.isDirectory()) continue;
      entry.getName(name, sizeof(name));
      entry.close();
      const std::string path = transfers + "/" + name;
      LOG_INF(TAG, "removing orphaned transfer %s", path.c_str());
      Storage.remove(path.c_str());
    }
  }
  if (dir) dir.close();

  // A complete record whose managed file is gone (removed over USB, card
  // swapped) goes back to remote-only; its acknowledgements are kept the way
  // an explicit removal keeps them.
  ManagedFile file;
  for (uint16_t i = 0; i < total; i++) {
    if (!read(i, file) || !file.complete()) continue;
    if (!Storage.exists(pathFor(file).c_str())) {
      LOG_INF(TAG, "managed file for book %lu missing; marking removed", static_cast<unsigned long>(file.bookId));
      file.flags &= static_cast<uint8_t>(~FILE_FLAG_COMPLETE);
      if (!upsert(file)) return false;
    }
  }
  return true;
}

}  // namespace bookorbit
