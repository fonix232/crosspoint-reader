#pragma once

#include <SecureHttpClient.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "BookOrbitCatalogParser.h"
#include "BookOrbitLibrarySelection.h"

namespace bookorbit {

enum class CatalogSort : uint8_t { Title, RecentlyRead };

enum class ClientError : uint8_t {
  OK = 0,
  NOT_CONFIGURED,
  NO_TRUST_ANCHOR,  // https without a usable CA and the insecure override off
  LOW_MEMORY,
  NETWORK,     // host unreachable (DNS, TCP, timeout)
  TLS_FAILED,  // TCP reached the host but the TLS handshake failed
  AUTH,        // 401: credentials rejected
  FORBIDDEN,   // 403: account disabled / sync disabled / permission revoked
  NOT_FOUND,
  SERVER,
  JSON,
  ABORTED,
  FILE,
  SIZE_MISMATCH,
  NOT_SUPPORTED,         // the server lacks the requested feature (older BookOrbit)
  LIBRARY_NOT_SELECTED,  // per-book skip; must not stop other queued downloads
};

const char* clientErrorName(ClientError error);

// What the server announces on `GET /koreader/plugin/version`
// (`capabilities: [...]`), the same list the KOReader plugin selects routes
// on. A server predating the route, or one that does not list a feature,
// gets the older path for it.
enum class ServerCapability : uint8_t {
  CatalogBulkManifest = 1u << 0,
  ReadingInsights = 1u << 1,   // /koreader/plugin/insights/*
  DeviceThumbnails = 1u << 2,  // thumbnail?w=&h=&depth=&format=
};

// Exact file identity for one downloadable EPUB, from the bulk manifest.
struct FileManifest {
  uint32_t bookId = 0;
  uint32_t fileId = 0;
  uint32_t sizeBytes = 0;  // 0 when the server does not know
  char contentVersion[33] = {};
  char fileHash[33] = {};  // KOReader partial MD5 of the exact bytes; empty when unknown
};

struct RemoteProgress {
  bool found = false;
  std::string locator;   // KOReader XPath-like progress string (may be empty)
  float percentage = 0;  // 0..1
  uint32_t timestamp = 0;
  std::string deviceId;
};

// HTTP client for the BookOrbit device API (`<origin>/api/v1/koreader/...`).
//
// One instance spans a network session so the kept-alive TLS connection is
// reused across catalogue pages, thumbnails, and progress calls. Requests are
// bounded: catalogue pages stream through CatalogPageParser, small JSON
// bodies are capped, and files stream straight to the card.
//
// TLS: https is verified against the PEM at CA_PEM_PATH when present, else
// the built-in ISRG Root X1. The store's insecure override disables
// verification for servers with private CAs when no PEM is provided. Note the
// SDK verifies the chain but not the host name (no wolfSSL_check_domain_name).
class Client {
 public:
  using ProgressCallback = bool (*)(void* ctx, size_t downloaded, size_t total);

  Client();
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Reads configuration and the trust anchor. Must succeed before any call.
  ClientError begin();
  // Drops the kept-alive connection (and its TLS buffers) without forgetting
  // the configuration; the next request reconnects. Use before heap-hungry
  // work such as chapter streaming or JPEG decoding.
  void disconnect();
  void end();

  ClientError authenticate();
  // Reads the server's version and capability list; a missing route (an older
  // BookOrbit) is not an error, it just leaves every capability unset.
  ClientError fetchCapabilities();
  bool hasCapability(ServerCapability capability) const {
    return (capabilities & static_cast<uint8_t>(capability)) != 0;
  }
  const std::string& serverVersion() const { return serverVersionText; }
  // Streams EPUB catalogue pages; offline catalogue sync keeps title order.
  ClientError fetchCatalogPage(uint16_t page, uint16_t size, CatalogPageParser& parser, const std::string& filters = {},
                               CatalogSort sort = CatalogSort::Title);
  ClientError checkBookSelection(uint32_t bookId, bool& selected);
  const LibrarySelection& librarySelection() const { return libraries; }
  // Picks the EPUB content file for `bookId` from the bulk manifest.
  ClientError fetchManifest(uint32_t bookId, FileManifest& out);
  // Saves the book thumbnail JPEG to `destPath` (replaced on success only).
  ClientError downloadThumbnail(uint32_t bookId, const std::string& destPath);
  // Asks the server for a cover rendered to `width` x `height` (fit inside),
  // 1-bit BMP, and streams it into `sink` (a Print) or `destPath` when `sink`
  // is null. NOT_SUPPORTED when the server ignored the request and served
  // its plain JPEG (a BookOrbit before device thumbnails); nothing is written
  // then and the transfer is dropped.
  ClientError downloadCoverBmp(uint32_t bookId, int width, int height, Print* sink, const std::string* destPath);
  // Streams the file bytes to `destPath`. `expectedSize` (when nonzero) must
  // match both Content-Length and the received byte count.
  ClientError downloadFile(uint32_t fileId, const std::string& destPath, uint32_t expectedSize,
                           ProgressCallback progress, void* progressCtx, const bool* cancelFlag);

  ClientError getProgress(const std::string& documentHash, RemoteProgress& out);
  // `timestamp` 0 omits the field (server assigns its own clock). `ackTimestamp`
  // receives the timestamp the server stored.
  ClientError putProgress(const std::string& documentHash, const std::string& locator, float percentage,
                          uint32_t timestamp, uint32_t& ackTimestamp);

  // GET of one small JSON document under the device API (`/koreader/...`),
  // collected whole (capped at MAX_JSON_BODY). Used for the insights routes.
  ClientError fetchJson(const std::string& path, std::string& body) { return getJson(path, body); }

  using DataCallback = bool (*)(void*, const char*, size_t);
  ClientError getStream(const std::string& path, DataCallback callback, void* context);
  ClientError postJson(const std::string& path, const std::string& request, std::string& response);
  ClientError postStream(const std::string& path, const std::string& request, DataCallback callback, void* context);

  int lastHttpCode() const { return lastCode; }

 private:
  static constexpr size_t MAX_JSON_BODY = 12 * 1024;

  ClientError fetchCatalogPageRaw(uint16_t page, uint16_t size, CatalogPageParser& parser, const std::string& filters,
                                  CatalogSort sort);
  LibrarySelection libraries;
  std::array<uint32_t, LibrarySelection::MAX> libraryTotals{};
  std::string totalsFilters;
  bool totalsValid = false;

  bool prepare(const std::string& path);
  ClientError mapStatus(int status);
  // For a request that produced no status line: on https, a plain TCP probe
  // separates an unreachable host from a failed handshake.
  ClientError transportError();
  ClientError insufficientHeap() const;
  // GET with the body collected into `body`, capped at MAX_JSON_BODY.
  ClientError getJson(const std::string& path, std::string& body);

  freeink::SecureHttpClient http;
  std::string apiBase;
  std::string user;
  std::string key;
  std::unique_ptr<char[]> caPem;  // SD-provided trust anchor, when present
  bool insecure = false;
  bool https = false;
  bool ready = false;
  int lastCode = 0;
  uint8_t capabilities = 0;
  std::string serverVersionText;
};

}  // namespace bookorbit
