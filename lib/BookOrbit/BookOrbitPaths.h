#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// SD layout for the BookOrbit integration. Every path below `accountDir()` is
// scoped to one account namespace (server origin + username), so switching
// accounts detaches the old cache, downloads, and acknowledgements instead of
// mixing them.
namespace bookorbit {

inline constexpr char CONFIG_PATH[] = "/.crosspoint/bookorbit.json";
// Optional PEM trust anchor for self-hosted servers. When present it replaces
// the built-in public root.
inline constexpr char CA_PEM_PATH[] = "/.crosspoint/bookorbit-ca.pem";
inline constexpr char ROOT_DIR[] = "/.crosspoint/bookorbit";

inline std::string accountDir(const std::string& accountKey) { return std::string(ROOT_DIR) + "/" + accountKey; }
inline std::string catalogPath(const std::string& accountKey) { return accountDir(accountKey) + "/catalog.bin"; }
inline std::string catalogStagingPath(const std::string& accountKey) {
  return accountDir(accountKey) + "/catalog.bin.tmp";
}
inline std::string catalogAuthorIndexPath(const std::string& accountKey) {
  return accountDir(accountKey) + "/catalog.author.idx";
}
inline std::string catalogRecentIndexPath(const std::string& accountKey) {
  return accountDir(accountKey) + "/catalog.recent.idx";
}
inline std::string catalogSeriesIndexPath(const std::string& accountKey) {
  return accountDir(accountKey) + "/catalog.series.idx";
}
inline std::string coversDir(const std::string& accountKey) { return accountDir(accountKey) + "/covers"; }
inline std::string coverTilePath(const std::string& accountKey, const uint32_t bookId) {
  char name[24];
  snprintf(name, sizeof(name), "/%lu.bw1", static_cast<unsigned long>(bookId));
  return coversDir(accountKey) + name;
}
// Source JPEG kept beside the tile, and the Library grid's 1-bit thumbnail
// rendered from it at a given height.
inline std::string coverJpegPath(const std::string& accountKey, const uint32_t bookId) {
  return coverTilePath(accountKey, bookId) + ".jpg";
}
inline std::string coverThumbPath(const std::string& accountKey, const uint32_t bookId, const int height) {
  char name[32];
  snprintf(name, sizeof(name), "/%lu_%d.bmp", static_cast<unsigned long>(bookId), height);
  return coversDir(accountKey) + name;
}
inline std::string filesIndexPath(const std::string& accountKey) { return accountDir(accountKey) + "/files.bin"; }
inline std::string filesIndexStagingPath(const std::string& accountKey) {
  return accountDir(accountKey) + "/files.bin.tmp";
}
inline std::string filesDir(const std::string& accountKey) { return accountDir(accountKey) + "/files"; }
// Managed downloads are named by stable server identity, never by title: the
// title lives in the catalogue cache and inside the EPUB itself.
inline std::string managedFilePath(const std::string& accountKey, const uint32_t bookId, const uint32_t fileId) {
  char name[40];
  snprintf(name, sizeof(name), "/%lu-%lu.epub", static_cast<unsigned long>(bookId), static_cast<unsigned long>(fileId));
  return filesDir(accountKey) + name;
}
inline std::string transfersDir(const std::string& accountKey) { return accountDir(accountKey) + "/transfers"; }
inline std::string stagingFilePath(const std::string& accountKey, const uint32_t fileId) {
  char name[32];
  snprintf(name, sizeof(name), "/%lu.part", static_cast<unsigned long>(fileId));
  return transfersDir(accountKey) + name;
}
inline std::string syncStatePath(const std::string& accountKey) { return accountDir(accountKey) + "/sync.json"; }

// True when `path` is a managed download under any account namespace.
inline bool isManagedPath(const std::string& path) {
  return path.rfind(ROOT_DIR, 0) == 0 && path[sizeof(ROOT_DIR) - 1] == '/' && path.find("/files/") != std::string::npos;
}

}  // namespace bookorbit
