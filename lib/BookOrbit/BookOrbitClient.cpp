#include "BookOrbitClient.h"

#include <ArduinoJson.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFiClient.h>
#include <esp_wifi.h>

#include <cstdio>
#include <cstring>

#include "BookOrbitClock.h"
#include "BookOrbitCredentialStore.h"
#include "BookOrbitDevice.h"
#include "BookOrbitPaths.h"
#include "BookOrbitRootCa.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "dev"
#endif

namespace bookorbit {

namespace {
constexpr char TAG[] = "BOAPI";
constexpr uint32_t API_TIMEOUT_MS = 30000;
constexpr uint32_t FILE_TIMEOUT_MS = 60000;
constexpr size_t MAX_CA_PEM = 8 * 1024;
// Same preflight margins as the KOSync client: not a guarantee, a floor
// below which a handshake is known to fail.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;
constexpr int32_t PROBE_TIMEOUT_MS = 5000;

// Keeps the radio out of modem sleep for the session's transfers; restored on
// destruction. The same pattern HttpDownloader applies per request.
struct PowerSaveGuard {
  PowerSaveGuard() { esp_wifi_set_ps(WIFI_PS_NONE); }
  ~PowerSaveGuard() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
};

void copyField(char* out, const size_t capacity, const char* value) {
  if (!value) {
    out[0] = '\0';
    return;
  }
  strncpy(out, value, capacity);
  out[capacity] = '\0';
}
}  // namespace

const char* clientErrorName(const ClientError error) {
  switch (error) {
    case ClientError::OK:
      return "ok";
    case ClientError::LIBRARY_NOT_SELECTED:
      return "library not selected";
    case ClientError::NOT_CONFIGURED:
      return "not configured";
    case ClientError::NO_TRUST_ANCHOR:
      return "no trust anchor";
    case ClientError::LOW_MEMORY:
      return "low memory";
    case ClientError::NETWORK:
      return "network";
    case ClientError::TLS_FAILED:
      return "tls";
    case ClientError::AUTH:
      return "auth";
    case ClientError::FORBIDDEN:
      return "forbidden";
    case ClientError::NOT_FOUND:
      return "not found";
    case ClientError::SERVER:
      return "server";
    case ClientError::JSON:
      return "json";
    case ClientError::ABORTED:
      return "aborted";
    case ClientError::FILE:
      return "file";
    case ClientError::SIZE_MISMATCH:
      return "size mismatch";
    case ClientError::NOT_SUPPORTED:
      return "not supported by server";
  }
  return "unknown";
}

Client::Client() = default;

Client::~Client() { end(); }

ClientError Client::begin() {
  auto& store = BOOKORBIT_STORE;
  if (!store.isConfigured()) return ClientError::NOT_CONFIGURED;
  libraries = store.getLibraries();
  totalsValid = false;
  apiBase = store.getApiBase();
  user = store.getUsername();
  key = store.getMd5Password();
  https = store.usesHttps();
  insecure = store.getInsecureTls();

  // Reading events need timestamps for every transport, including plain HTTP.
  const bool clockReady = ensureSystemClock();
  caPem.reset();
  if (https && !insecure) {
    // Certificate validity windows are checked against the process clock.
    if (!clockReady) {
      LOG_ERR(TAG, "cannot verify certificates without a valid clock");
      return ClientError::NO_TRUST_ANCHOR;
    }
    HalFile pem;
    if (Storage.exists(CA_PEM_PATH) && Storage.openFileForRead(TAG, CA_PEM_PATH, pem)) {
      const size_t size = pem.fileSize();
      if (size == 0 || size > MAX_CA_PEM) {
        LOG_ERR(TAG, "CA PEM rejected (%u bytes)", static_cast<unsigned>(size));
        return ClientError::NO_TRUST_ANCHOR;
      }
      auto buf = makeUniqueNoThrow<char[]>(size + 1);
      if (!buf) {
        LOG_ERR(TAG, "OOM: %u-byte CA PEM", static_cast<unsigned>(size));
        return ClientError::LOW_MEMORY;
      }
      if (pem.read(buf.get(), size) != static_cast<int>(size)) return ClientError::FILE;
      buf[size] = '\0';
      if (!strstr(buf.get(), "-----BEGIN CERTIFICATE-----")) {
        LOG_ERR(TAG, "CA PEM has no certificate block");
        return ClientError::NO_TRUST_ANCHOR;
      }
      caPem = std::move(buf);
      LOG_INF(TAG, "Using CA from %s", CA_PEM_PATH);
    }
  }

  LOG_INF(TAG, "server %s as %s", apiBase.c_str(), user.c_str());
  http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
  // Keep redirects disabled: SDK redirects forward custom authentication headers.
  http.setReuse(true);
  if (https) {
    if (insecure) {
      LOG_INF(TAG, "TLS verification disabled by setting");
      http.setInsecure();
    } else {
      http.setCACert(caPem ? caPem.get() : ISRG_ROOT_X1_PEM);
    }
  }
  ready = true;
  return ClientError::OK;
}

void Client::disconnect() { http.end(); }

void Client::end() {
  http.end();
  ready = false;
}

ClientError Client::insufficientHeap() const {
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes < MIN_FREE_FOR_TLS || heap.largestBlockBytes < MIN_BLOCK_FOR_TLS) {
    LOG_ERR(TAG, "Heap too low for TLS: %u free, %u largest", static_cast<unsigned>(heap.freeBytes),
            static_cast<unsigned>(heap.largestBlockBytes));
    return ClientError::LOW_MEMORY;
  }
  return ClientError::OK;
}

bool Client::prepare(const std::string& path) {
  if (!ready) return false;
  const std::string url = apiBase + path;
  if (!http.begin(url)) {
    LOG_ERR(TAG, "Bad URL: %s", url.c_str());
    return false;
  }
  http.setTimeout(API_TIMEOUT_MS);
  http.addHeader("Accept", "application/json");
  http.addHeader("x-auth-user", user);
  http.addHeader("x-auth-key", key);
  return true;
}

ClientError Client::transportError() {
  if (!https) return ClientError::NETWORK;
  // apiBase is "<scheme>://<host>[:port]/api/v1"; SecureHttpClient already
  // accepted it, so the shape is known.
  const size_t hostStart = apiBase.find("://") + 3;
  const size_t hostEnd = apiBase.find('/', hostStart);
  std::string host = apiBase.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
  uint16_t port = 443;
  const size_t colon = host.rfind(':');
  if (colon != std::string::npos) {
    port = static_cast<uint16_t>(atoi(host.c_str() + colon + 1));
    host.erase(colon);
  }
  WiFiClient probe;
  if (!probe.connect(host.c_str(), port, PROBE_TIMEOUT_MS)) {
    LOG_ERR(TAG, "TCP probe to %s:%u failed", host.c_str(), port);
    return ClientError::NETWORK;
  }
  probe.stop();
  LOG_ERR(TAG, "%s:%u reachable; TLS handshake failed (see [SecureClient] lines: certificate, hash, or clock)",
          host.c_str(), port);
  return ClientError::TLS_FAILED;
}

ClientError Client::mapStatus(const int status) {
  if (status < 0) return transportError();
  if (status >= 200 && status < 300) return ClientError::OK;
  if (status == 401) return ClientError::AUTH;
  if (status == 403) return ClientError::FORBIDDEN;
  if (status == 404) return ClientError::NOT_FOUND;
  return ClientError::SERVER;
}

ClientError Client::getJson(const std::string& path, std::string& body) {
  if (!ready) return ClientError::NOT_CONFIGURED;
  if (const auto heap = insufficientHeap(); heap != ClientError::OK) return heap;
  if (!prepare(path)) return ClientError::NETWORK;
  body.clear();
  bool overflow = false;
  const int status = http.GET([&body, &overflow](const uint8_t* data, const size_t len) {
    if (body.size() + len > MAX_JSON_BODY) {
      overflow = true;
      return false;
    }
    body.append(reinterpret_cast<const char*>(data), len);
    return true;
  });
  lastCode = status;
  LOG_DBG(TAG, "GET %s -> %d (%u bytes)", path.c_str(), status, static_cast<unsigned>(body.size()));
  if (overflow) {
    LOG_ERR(TAG, "Response over %u bytes: %s", static_cast<unsigned>(MAX_JSON_BODY), path.c_str());
    return ClientError::JSON;
  }
  const ClientError mapped = mapStatus(status);
  if (mapped == ClientError::OK && !http.responseComplete()) return ClientError::NETWORK;
  if (mapped != ClientError::OK && status > 0) {
    // The server's own reason (bounded, printable ASCII only) tells a rejected
    // credential apart from a route served by the wrong guard or a proxy page.
    char reason[161];
    size_t n = 0;
    for (size_t i = 0; i < body.size() && n < sizeof(reason) - 1; i++) {
      const unsigned char c = static_cast<unsigned char>(body[i]);
      reason[n++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
    }
    reason[n] = '\0';
    LOG_ERR(TAG, "%s -> %d: %s", path.c_str(), status, reason);
  }
  return mapped;
}

ClientError Client::authenticate() {
  std::string body;
  return getJson("/koreader/users/auth", body);
}

ClientError Client::fetchCapabilities() {
  capabilities = 0;
  serverVersionText.clear();
  std::string body;
  const ClientError err = getJson("/koreader/plugin/version", body);
  if (err == ClientError::NOT_FOUND) {
    LOG_INF(TAG, "server has no version route; base API only");
    return ClientError::OK;
  }
  if (err != ClientError::OK) return err;
  JsonDocument filter;
  filter["serverVersion"] = true;
  filter["capabilities"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return ClientError::JSON;
  serverVersionText = doc["serverVersion"] | "";
  for (const JsonVariantConst item : doc["capabilities"].as<JsonArrayConst>()) {
    const char* name = item.as<const char*>();
    if (!name) continue;
    if (strcmp(name, "catalogBulkManifest") == 0)
      capabilities |= static_cast<uint8_t>(ServerCapability::CatalogBulkManifest);
    if (strcmp(name, "readingInsights") == 0) capabilities |= static_cast<uint8_t>(ServerCapability::ReadingInsights);
    if (strcmp(name, "deviceThumbnails") == 0) capabilities |= static_cast<uint8_t>(ServerCapability::DeviceThumbnails);
  }
  LOG_INF(TAG, "server %s, capabilities 0x%02x", serverVersionText.c_str(), capabilities);
  return ClientError::OK;
}

ClientError Client::fetchCatalogPage(const uint16_t page, const uint16_t size, CatalogPageParser& parser,
                                     const std::string& filters, const CatalogSort sort) {
  if (!ready) return ClientError::NOT_CONFIGURED;
  if (libraries.all) return fetchCatalogPageRaw(page, size, parser, filters, sort);
  if (libraries.count == 1)
    return fetchCatalogPageRaw(page, size, parser, filters + "&libraryId=" + std::to_string(libraries.ids[0]), sort);
  parser.reset();
  if (!page || !size || size > 100) return ClientError::JSON;
  if (!libraries.count) {
    parser.setPageMetadata(0, false);
    return ClientError::OK;
  }
  // The server accepts one libraryId. Concatenate its filtered pages without
  // buffering books in RAM. The offline writer sorts the merged catalogue.
  if (!totalsValid || totalsFilters != filters) {
    totalsValid = false;
    auto counter = makeUniqueNoThrow<CatalogPageParser>(nullptr, nullptr);
    if (!counter) return ClientError::LOW_MEMORY;
    for (unsigned i = 0; i < libraries.count; ++i) {
      auto error =
          fetchCatalogPageRaw(1, 1, *counter, filters + "&libraryId=" + std::to_string(libraries.ids[i]), sort);
      if (error != ClientError::OK) return error;
      libraryTotals[i] = counter->total();
    }
    totalsFilters = filters;
    totalsValid = true;
  }
  uint32_t total = 0;
  for (unsigned i = 0; i < libraries.count; ++i) total += libraryTotals[i];
  uint32_t skip = static_cast<uint32_t>(page - 1) * size;
  parser.setPageMetadata(total, skip + size < total);
  struct Sink {
    CatalogPageParser* target;
    unsigned skip = 0, remaining = 0;
  } sink{&parser, 0, size};
  auto part = makeUniqueNoThrow<CatalogPageParser>(
      [](void* ctx, const CatalogRecord& record) {
        auto& s = *static_cast<Sink*>(ctx);
        if (s.skip) {
          --s.skip;
          return true;
        }
        if (!s.remaining) return true;
        --s.remaining;
        return s.target->emitRecord(record);
      },
      &sink);
  if (!part) return ClientError::LOW_MEMORY;
  for (unsigned i = 0; i < libraries.count && sink.remaining; ++i) {
    if (skip >= libraryTotals[i]) {
      skip -= libraryTotals[i];
      continue;
    }
    uint32_t local = skip;
    skip = 0;
    while (local < libraryTotals[i] && sink.remaining) {
      sink.skip = local % size;
      const auto error = fetchCatalogPageRaw(local / size + 1, size, *part,
                                             filters + "&libraryId=" + std::to_string(libraries.ids[i]), sort);
      if (error != ClientError::OK) return error;
      if (part->itemCount() == 0) {
        totalsValid = false;
        return ClientError::JSON;
      }
      local = (local / size + 1) * size;
    }
  }
  return ClientError::OK;
}

ClientError Client::checkBookSelection(const uint32_t bookId, bool& selected) {
  selected = libraries.all;
  if (libraries.all || !libraries.count) return ClientError::OK;
  // Details can include large descriptions. Stream only the root libraryId
  // instead of collecting the whole JSON document in RAM.
  struct Membership {
    unsigned depth = 0;
    uint32_t id = 0;
    bool field = false, ended = false, overflow = false;
    StreamingJsonParser parser;
    Membership()
        : parser(JsonCallbacks{this,
                               [](void* ctx, const char* key, size_t len) {
                                 auto& s = *static_cast<Membership*>(ctx);
                                 s.field = s.depth == 1 && len == 9 && memcmp(key, "libraryId", 9) == 0;
                               },
                               nullptr,
                               [](void* ctx, const char* value, size_t len) {
                                 auto& s = *static_cast<Membership*>(ctx);
                                 if (!s.field || s.depth != 1 || len == 0 || len > 10) return;
                                 uint64_t id = 0;
                                 for (size_t i = 0; i < len; ++i) {
                                   if (value[i] < '0' || value[i] > '9') return;
                                   id = id * 10 + value[i] - '0';
                                 }
                                 if (id <= UINT32_MAX) s.id = id;
                                 s.field = false;
                               },
                               nullptr, nullptr, [](void* ctx) { ++static_cast<Membership*>(ctx)->depth; },
                               [](void* ctx) {
                                 auto& s = *static_cast<Membership*>(ctx);
                                 if (--s.depth == 0) s.ended = true;
                               },
                               nullptr, nullptr,
                               [](void* ctx) {
                                 auto& s = *static_cast<Membership*>(ctx);
                                 if (s.field) s.overflow = true;
                               }}) {}
  };
  auto membership = makeUniqueNoThrow<Membership>();
  if (!membership) return ClientError::LOW_MEMORY;
  const auto error = getStream(
      "/koreader/plugin/catalog/books/" + std::to_string(bookId),
      [](void* ctx, const char* bytes, size_t len) {
        auto& s = *static_cast<Membership*>(ctx);
        s.parser.feed(bytes, len);
        return !s.parser.hasError() && !s.overflow;
      },
      membership.get());
  if (error != ClientError::OK) return error;
  if (!membership->ended || membership->overflow || membership->parser.hasError() || !membership->id)
    return ClientError::JSON;
  selected = libraries.contains(membership->id);
  return ClientError::OK;
}

ClientError Client::fetchCatalogPageRaw(const uint16_t page, const uint16_t size, CatalogPageParser& parser,
                                        const std::string& filters, const CatalogSort sort) {
  if (!ready) return ClientError::NOT_CONFIGURED;
  if (const auto heap = insufficientHeap(); heap != ClientError::OK) return heap;
  char query[128];
  snprintf(query, sizeof(query), "/koreader/plugin/catalog/books?page=%u&size=%u&format=epub&sort=%s&order=%s", page,
           size, sort == CatalogSort::RecentlyRead ? "recently_read" : "title",
           sort == CatalogSort::RecentlyRead ? "desc" : "asc");
  if (!prepare(std::string(query) + filters)) return ClientError::NETWORK;
  parser.reset();
  const int status = http.GET([this, &parser](const uint8_t* data, const size_t len) {
    if (http.getStatus() != 200) return true;  // error bodies are not catalogue pages
    parser.feed(reinterpret_cast<const char*>(data), len);
    return !parser.hasError() && !parser.aborted();
  });
  lastCode = status;
  LOG_DBG(TAG, "catalogue page %u -> %d, %u items", page, status, parser.itemCount());
  const ClientError mapped = mapStatus(status);
  if (mapped != ClientError::OK) return mapped;
  if (parser.aborted()) return ClientError::ABORTED;
  if (!parser.complete() || !http.responseComplete()) return ClientError::JSON;
  return ClientError::OK;
}

ClientError Client::fetchManifest(const uint32_t bookId, FileManifest& out) {
  char path[96];
  snprintf(path, sizeof(path), "/koreader/plugin/catalog/manifest?ids=%lu&size=1&format=epub",
           static_cast<unsigned long>(bookId));
  std::string body;
  const ClientError err = getJson(path, body);
  if (err != ClientError::OK) return err;

  // Only the file identity fields are kept; titles and URLs are dropped at
  // parse time so the document stays small.
  JsonDocument filter;
  filter["items"][0]["id"] = true;
  filter["items"][0]["files"][0]["id"] = true;
  filter["items"][0]["files"][0]["format"] = true;
  filter["items"][0]["files"][0]["sizeBytes"] = true;
  filter["items"][0]["files"][0]["contentVersion"] = true;
  filter["items"][0]["files"][0]["fileHash"] = true;
  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  body.clear();
  if (parseError) {
    LOG_ERR(TAG, "manifest parse: %s", parseError.c_str());
    return ClientError::JSON;
  }
  for (JsonObjectConst item : doc["items"].as<JsonArrayConst>()) {
    if (item["id"].as<uint32_t>() != bookId) continue;
    for (JsonObjectConst file : item["files"].as<JsonArrayConst>()) {
      const char* format = file["format"] | "";
      if (strcasecmp(format, "epub") != 0) continue;
      out = FileManifest{};
      out.bookId = bookId;
      out.fileId = file["id"].as<uint32_t>();
      out.sizeBytes = file["sizeBytes"].as<uint32_t>();
      copyField(out.contentVersion, sizeof(out.contentVersion) - 1, file["contentVersion"] | "");
      copyField(out.fileHash, sizeof(out.fileHash) - 1, file["fileHash"] | "");
      if (out.fileId == 0) return ClientError::JSON;
      return ClientError::OK;
    }
  }
  return ClientError::NOT_FOUND;
}

ClientError Client::downloadThumbnail(const uint32_t bookId, const std::string& destPath) {
  if (!ready) return ClientError::NOT_CONFIGURED;
  if (const auto heap = insufficientHeap(); heap != ClientError::OK) return heap;
  char path[80];
  snprintf(path, sizeof(path), "/koreader/plugin/catalog/books/%lu/thumbnail", static_cast<unsigned long>(bookId));
  if (!prepare(path)) return ClientError::NETWORK;

  const std::string tmp = destPath + ".tmp";
  Storage.remove(tmp.c_str());
  HalFile file;
  if (!Storage.openFileForWrite(TAG, tmp.c_str(), file)) return ClientError::FILE;
  bool writeFailed = false;
  size_t magicBytes = 0;
  uint16_t endMarker = 0;
  bool isJpeg = true;
  const int status = http.GET([&](const uint8_t* data, const size_t len) {
    if (http.getStatus() != 200) return true;
    for (size_t i = 0; i < len && magicBytes < 2; ++i, ++magicBytes) {
      if (data[i] != (magicBytes == 0 ? 0xFF : 0xD8)) {
        isJpeg = false;
        return false;
      }
    }
    if (len >= 2)
      endMarker = (static_cast<uint16_t>(data[len - 2]) << 8) | data[len - 1];
    else if (len == 1)
      endMarker = static_cast<uint16_t>((endMarker << 8) | data[0]);
    if (file.write(data, len) != len) {
      writeFailed = true;
      return false;
    }
    return true;
  });
  if (!file.close()) writeFailed = true;
  lastCode = status;
  ClientError result = mapStatus(status);
  if (result == ClientError::OK) {
    if (writeFailed) {
      result = ClientError::FILE;
    } else if (!isJpeg || magicBytes != 2 || endMarker != 0xFFD9) {
      LOG_ERR(TAG, "thumbnail %lu is not a complete JPEG", static_cast<unsigned long>(bookId));
      result = ClientError::JSON;
    } else if (!http.responseComplete()) {
      result = ClientError::NETWORK;
    }
  }
  if (result != ClientError::OK) {
    Storage.remove(tmp.c_str());
    return result;
  }
  Storage.remove(destPath.c_str());
  if (!Storage.rename(tmp.c_str(), destPath.c_str())) {
    Storage.remove(tmp.c_str());
    return ClientError::FILE;
  }
  return ClientError::OK;
}

ClientError Client::downloadCoverBmp(const uint32_t bookId, const int width, const int height, Print* sink,
                                     const std::string* destPath) {
  if (!ready) return ClientError::NOT_CONFIGURED;
  if (const auto heap = insufficientHeap(); heap != ClientError::OK) return heap;
  char path[128];
  snprintf(path, sizeof(path), "/koreader/plugin/catalog/books/%lu/thumbnail?w=%d&h=%d&depth=1&format=bmp",
           static_cast<unsigned long>(bookId), width, height);
  if (!prepare(path)) return ClientError::NETWORK;

  std::string tmp;
  HalFile file;
  if (!sink) {
    if (!destPath) return ClientError::FILE;
    tmp = *destPath + ".tmp";
    Storage.remove(tmp.c_str());
    if (!Storage.openFileForWrite(TAG, tmp.c_str(), file)) return ClientError::FILE;
  }
  bool writeFailed = false;
  size_t magicBytes = 0;
  bool isBmp = true;
  const int status = http.GET([&](const uint8_t* data, const size_t len) {
    if (http.getStatus() != 200) return true;
    for (size_t i = 0; i < len && magicBytes < 2; ++i, ++magicBytes) {
      if (data[i] != (magicBytes == 0 ? 'B' : 'M')) {
        isBmp = false;
        return false;
      }
    }
    const size_t written = sink ? sink->write(data, len) : file.write(data, len);
    if (written != len) {
      writeFailed = true;
      return false;
    }
    return true;
  });
  if (!sink && !file.close()) writeFailed = true;
  lastCode = status;
  ClientError result = mapStatus(status);
  if (result == ClientError::OK) {
    if (!isBmp) {
      result = ClientError::NOT_SUPPORTED;
    } else if (writeFailed) {
      result = ClientError::FILE;
    } else if (magicBytes != 2) {
      result = ClientError::JSON;
    } else if (!http.responseComplete()) {
      result = ClientError::NETWORK;
    }
  }
  if (sink) return result;
  if (result != ClientError::OK) {
    Storage.remove(tmp.c_str());
    return result;
  }
  Storage.remove(destPath->c_str());
  if (!Storage.rename(tmp.c_str(), destPath->c_str())) {
    Storage.remove(tmp.c_str());
    return ClientError::FILE;
  }
  return ClientError::OK;
}

ClientError Client::downloadFile(const uint32_t fileId, const std::string& destPath, const uint32_t expectedSize,
                                 const ProgressCallback progress, void* progressCtx, const bool* cancelFlag) {
  if (!ready) return ClientError::NOT_CONFIGURED;
  if (const auto heap = insufficientHeap(); heap != ClientError::OK) return heap;
  char path[80];
  snprintf(path, sizeof(path), "/koreader/plugin/catalog/files/%lu/download", static_cast<unsigned long>(fileId));
  if (!prepare(path)) return ClientError::NETWORK;
  http.setTimeout(FILE_TIMEOUT_MS);

  Storage.remove(destPath.c_str());
  HalFile file;
  if (!Storage.openFileForWrite(TAG, destPath.c_str(), file)) return ClientError::FILE;

  PowerSaveGuard powerSave;
  bool writeFailed = false;
  bool lengthMismatch = false;
  bool progressAborted = false;
  size_t received = 0;
  const int status = http.GET(
      [&](const uint8_t* data, const size_t len) {
        if (http.getStatus() != 200) return true;
        if (expectedSize > 0 && http.hasContentLength() && http.getContentLength() != expectedSize) {
          lengthMismatch = true;
          return false;
        }
        if (file.write(data, len) != len) {
          writeFailed = true;
          return false;
        }
        received += len;
        if (progress && !progress(progressCtx, received, http.hasContentLength() ? http.getContentLength() : 0)) {
          progressAborted = true;
          return false;
        }
        return true;
      },
      [cancelFlag]() { return cancelFlag && *cancelFlag; });
  file.flush();
  if (!file.close()) writeFailed = true;
  lastCode = status;

  ClientError result = mapStatus(status);
  if (http.aborted() || progressAborted) {
    result = ClientError::ABORTED;
  } else if (result == ClientError::OK) {
    if (lengthMismatch) {
      LOG_ERR(TAG, "file %lu: Content-Length differs from manifest size %lu", static_cast<unsigned long>(fileId),
              static_cast<unsigned long>(expectedSize));
      result = ClientError::SIZE_MISMATCH;
    } else if (writeFailed) {
      result = ClientError::FILE;
    } else if (!http.responseComplete() || received == 0) {
      LOG_ERR(TAG, "file %lu incomplete after %u bytes", static_cast<unsigned long>(fileId),
              static_cast<unsigned>(received));
      result = ClientError::NETWORK;
    } else if (expectedSize > 0 && received != expectedSize) {
      LOG_ERR(TAG, "file %lu: received %u bytes, expected %lu", static_cast<unsigned long>(fileId),
              static_cast<unsigned>(received), static_cast<unsigned long>(expectedSize));
      result = ClientError::SIZE_MISMATCH;
    }
  }
  if (result != ClientError::OK) Storage.remove(destPath.c_str());
  return result;
}

ClientError Client::getProgress(const std::string& documentHash, RemoteProgress& out) {
  out = RemoteProgress{};
  std::string body;
  const ClientError err = getJson("/koreader/syncs/progress/" + documentHash, body);
  if (err == ClientError::NOT_FOUND) return ClientError::OK;  // no record: found stays false
  if (err != ClientError::OK) return err;
  if (lastCode == 204) return ClientError::OK;
  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, body);
  body.clear();
  if (parseError) {
    LOG_ERR(TAG, "progress parse: %s", parseError.c_str());
    return ClientError::JSON;
  }
  // BookOrbit answers `{}` when nothing is stored for the hash.
  if (doc["document"].isNull() && doc["percentage"].isNull()) return ClientError::OK;
  out.found = true;
  const char* locator = doc["progress"] | "";
  out.locator = locator;
  out.percentage = doc["percentage"].as<float>();
  out.timestamp = doc["timestamp"].as<uint32_t>();
  out.deviceId = doc["device_id"] | "";
  return ClientError::OK;
}

ClientError Client::putProgress(const std::string& documentHash, const std::string& locator, const float percentage,
                                const uint32_t timestamp, uint32_t& ackTimestamp) {
  if (!ready) return ClientError::NOT_CONFIGURED;
  if (const auto heap = insufficientHeap(); heap != ClientError::OK) return heap;
  JsonDocument doc;
  doc["document"] = documentHash;
  doc["progress"] = locator;
  doc["percentage"] = percentage < 0 ? 0.0f : percentage > 1 ? 1.0f : percentage;
  doc["device"] = deviceName();
  doc["device_id"] = BOOKORBIT_STORE.getDeviceId();
  if (timestamp > 0) doc["timestamp"] = timestamp;
  std::string body;
  serializeJson(doc, body);
  doc.clear();

  if (!prepare("/koreader/syncs/progress")) return ClientError::NETWORK;
  http.addHeader("Content-Type", "application/json");
  const int status = http.sendRequest("PUT", body);
  lastCode = status;
  LOG_DBG(TAG, "PUT progress -> %d", status);
  const ClientError mapped = mapStatus(status);
  if (mapped != ClientError::OK) return mapped;
  ackTimestamp = timestamp;
  JsonDocument reply;
  if (!deserializeJson(reply, http.getString()) && !reply["timestamp"].isNull()) {
    ackTimestamp = reply["timestamp"].as<uint32_t>();
  }
  return ClientError::OK;
}

ClientError Client::getStream(const std::string& path, DataCallback callback, void* context) {
  if (!ready) return ClientError::NOT_CONFIGURED;
  if (const auto heap = insufficientHeap(); heap != ClientError::OK) return heap;
  if (!prepare(path)) return ClientError::NETWORK;
  const int status = http.GET([this, callback, context](const uint8_t* bytes, size_t size) {
    return http.getStatus() != 200 || callback(context, reinterpret_cast<const char*>(bytes), size);
  });
  lastCode = status;
  const auto error = mapStatus(status);
  return error != ClientError::OK ? error : (http.responseComplete() ? ClientError::OK : ClientError::JSON);
}

ClientError Client::postStream(const std::string& path, const std::string& request, DataCallback callback,
                               void* context) {
  if (!ready) return ClientError::NOT_CONFIGURED;
  if (request.size() > MAX_JSON_BODY) return ClientError::LOW_MEMORY;
  if (const auto heap = insufficientHeap(); heap != ClientError::OK) return heap;
  if (!prepare(path)) return ClientError::NETWORK;
  http.addHeader("Content-Type", "application/json");
  const int status = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(request.data()), request.size(),
                                      [this, callback, context](const uint8_t* data, size_t size) {
                                        if (http.getStatus() < 200 || http.getStatus() >= 300) return true;
                                        return callback(context, reinterpret_cast<const char*>(data), size);
                                      });
  lastCode = status;
  const auto error = mapStatus(status);
  if (error != ClientError::OK) return error;
  return http.responseComplete() ? ClientError::OK : ClientError::JSON;
}

ClientError Client::postJson(const std::string& path, const std::string& request, std::string& response) {
  response.clear();
  return postStream(
      path, request,
      [](void* context, const char* data, size_t size) {
        auto& body = *static_cast<std::string*>(context);
        if (size > MAX_JSON_BODY - body.size()) return false;
        body.append(data, size);
        return true;
      },
      &response);
}

}  // namespace bookorbit
