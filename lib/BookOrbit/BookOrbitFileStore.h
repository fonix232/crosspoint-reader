#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

namespace bookorbit {

inline constexpr size_t FILE_RECORD_BYTES = 128;
inline constexpr size_t FILE_VERSION_BYTES = 32;
inline constexpr size_t FILE_HASH_BYTES = 32;
inline constexpr uint16_t FILE_STORE_MAX = 512;

inline constexpr uint8_t FILE_FLAG_COMPLETE = 1u << 0;
inline constexpr uint8_t FILE_FLAG_ACK_LOCAL = 1u << 1;
inline constexpr uint8_t FILE_FLAG_ACK_REMOTE = 1u << 2;
// Both sides moved since the last agreement and no screen was available to
// ask; the foreground sync session resolves it.
inline constexpr uint8_t FILE_FLAG_CONFLICT = 1u << 3;
inline constexpr uint8_t FILE_FLAG_SIDELOAD = 1u << 4;

// One managed download: which server file sits at the managed path, the
// identity it was verified against, and the last progress exchange both
// sides agreed on. The path is derived from the ids, never stored.
struct ManagedFile {
  uint32_t bookId = 0;
  uint32_t fileId = 0;
  uint32_t sizeBytes = 0;
  uint32_t downloadedAt = 0;
  uint8_t flags = 0;
  // Local position acknowledged by the server (spine + content key).
  uint16_t ackLocalSpine = 0;
  uint32_t ackLocalKey = 0;
  // Remote record acknowledged locally (server timestamp + locator hash).
  uint32_t ackRemoteTs = 0;
  uint32_t ackRemoteHash = 0;
  char contentVersion[FILE_VERSION_BYTES + 1] = {};
  char fileHash[FILE_HASH_BYTES + 1] = {};

  bool complete() const { return (flags & FILE_FLAG_COMPLETE) != 0; }
  bool hasLocalAck() const { return (flags & FILE_FLAG_ACK_LOCAL) != 0; }
  bool hasRemoteAck() const { return (flags & FILE_FLAG_ACK_REMOTE) != 0; }
  bool hasConflict() const { return (flags & FILE_FLAG_CONFLICT) != 0; }
};

void encodeManagedFile(const ManagedFile& in, uint8_t out[FILE_RECORD_BYTES]);
void decodeManagedFile(const uint8_t in[FILE_RECORD_BYTES], ManagedFile& out);

// files.bin: fixed records, one per managed book. Lookups go through a
// resident bookId array (4 bytes per record, at most 2 KiB);
// records themselves are read from the card on demand. Writes rewrite the
// file through a staging copy so an interrupted write never tears the live
// index.
class FileStore {
 public:
  FileStore() = default;
  FileStore(const FileStore&) = delete;
  FileStore& operator=(const FileStore&) = delete;

  bool open(const std::string& accountKey);
  void close();
  bool isOpen() const { return opened; }
  uint16_t count() const { return opened ? total : 0; }

  int indexOfBook(uint32_t bookId) const;
  bool read(uint16_t index, ManagedFile& out) const;
  bool find(uint32_t bookId, ManagedFile& out) const;
  // True when the record says a complete managed file is on the card. Answered
  // from the resident flags, so it is safe to call per rendered row.
  bool isComplete(uint32_t bookId) const;
  bool hasConflict(uint32_t bookId) const;
  std::string pathFor(const ManagedFile& file) const;

  bool attachSideload(ManagedFile file, const std::string& path);
  bool upsert(const ManagedFile& file);
  bool remove(uint32_t bookId);
  // "Remove from device": deletes the bytes but keeps the record with its
  // acknowledgements, so a later download of the same version can keep the
  // reader's saved position instead of treating it as a fresh book.
  bool markRemoved(uint32_t bookId);

  // Drops complete records whose file vanished and deletes orphaned staging
  // files. Run at the start of a session, before any transfer.
  bool reconcile();

 private:
  bool rewrite(const ManagedFile* replacement, uint32_t removeBookId);
  bool loadIds();

  bool opened = false;
  std::string account;
  uint16_t total = 0;
  std::unique_ptr<uint32_t[]> ids;
  std::unique_ptr<uint8_t[]> flags;
};

}  // namespace bookorbit
