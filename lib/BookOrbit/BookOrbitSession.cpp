#include "BookOrbitSession.h"

#include <Epub.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <KOReaderDocumentId.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <PersistableStore.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "BookOrbitAnnotationSync.h"
#include "BookOrbitCatalogCache.h"
#include "BookOrbitCatalogParser.h"
#include "BookOrbitCoverTile.h"
#include "BookOrbitCredentialStore.h"
#include "BookOrbitDataProtocol.h"
#include "BookOrbitInsights.h"
#include "BookOrbitInsightsApi.h"
#include "BookOrbitInsightsCache.h"
#include "BookOrbitPaths.h"
#include "BookOrbitProgressPolicy.h"
#include "BookOrbitReadingTracker.h"
#include "BookOrbitStorage.h"
#include "BookOrbitSyncState.h"

namespace bookorbit {

namespace {
constexpr char TAG[] = "BOSES";

// Catalogue page sink: appends to the writer up to the cache cap. Items past
// the cap are skipped rather than aborting the page, so the page's `total`
// (which follows `items` in the body) is still read.
struct PageSink {
  CatalogWriter* writer;
  bool full = false;
  static bool onItem(void* ctx, const CatalogRecord& record) {
    auto* self = static_cast<PageSink*>(ctx);
    if (self->writer->count() >= CATALOG_MAX_BOOKS) {
      self->full = true;
      return true;
    }
    return self->writer->append(record);
  }
};

void acknowledge(ManagedFile& file, const LocalProgressFile& local, const uint32_t remoteTs,
                 const uint32_t remoteHash) {
  file.flags |= FILE_FLAG_ACK_LOCAL | FILE_FLAG_ACK_REMOTE;
  file.flags &= static_cast<uint8_t>(~FILE_FLAG_CONFLICT);
  file.ackLocalSpine = local.spine;
  file.ackLocalKey = local.toState().key;
  file.ackRemoteTs = remoteTs;
  file.ackRemoteHash = remoteHash;
}

bool equalsIgnoreCase(const char* a, const char* b) { return strcasecmp(a, b) == 0; }

// Managed files are always EPUBs, so the reader cache to drop is the EPUB one.
void clearBookCache(const std::string& path) { Epub(path, "/.crosspoint").clearCache(); }
}  // namespace

Session::Session(Client& client, FileStore& files, GfxRenderer& renderer, std::string account,
                 const bool lendFramebuffer, const SessionCallbacks callbacks)
    : client_(client),
      files_(files),
      renderer_(renderer),
      account_(std::move(account)),
      lendFramebuffer_(lendFramebuffer),
      callbacks_(callbacks) {}

void Session::phase(const SessionPhase phase, const uint32_t a, const uint32_t b) {
  if (callbacks_.onPhase) callbacks_.onPhase(callbacks_.ctx, phase, a, b);
}

ClientError Session::connect() {
  phase(SessionPhase::Connecting);
  ClientError error = client_.begin();
  if (error != ClientError::OK) return error;
  error = client_.authenticate();
  if (error != ClientError::OK) return error;
  // Non-fatal: a server that cannot say leaves every optional path off.
  if (client_.fetchCapabilities() != ClientError::OK) LOG_ERR(TAG, "capability query failed; base API only");
  Storage.mkdir(accountDir(account_).c_str());
  if (!files_.open(account_) || !files_.reconcile()) return ClientError::FILE;
  return ClientError::OK;
}

// --- catalogue ---------------------------------------------------------------

ClientError Session::refreshCatalog(const uint32_t fetchedAt, CatalogResult& out, void (*beforeInstall)(void*),
                                    void* beforeInstallCtx) {
  out = CatalogResult{};
  CatalogWriter writer;
  if (!writer.begin(account_)) return ClientError::FILE;
  PageSink sink{&writer};
  CatalogPageParser parser(&PageSink::onItem, &sink);
  ClientError error = ClientError::OK;
  phase(SessionPhase::Catalog);
  for (uint16_t page = 1;; page++) {
    if (cancelled()) {
      writer.abort();
      return ClientError::ABORTED;
    }
    error = client_.fetchCatalogPage(page, CATALOG_PAGE_SIZE, parser);
    if (error != ClientError::OK) {
      // The parser stops only when a staged write fails.
      if (error == ClientError::ABORTED) error = ClientError::FILE;
      LOG_ERR(TAG, "catalogue page %u failed: %s (http %d)", page, clientErrorName(error), client_.lastHttpCode());
      writer.abort();
      return error;
    }
    out.serverTotal = parser.total();
    phase(SessionPhase::Catalog, writer.count(), std::min<uint32_t>(out.serverTotal, CATALOG_MAX_BOOKS));
    if (sink.full) {
      out.truncated = true;
      break;
    }
    if (!parser.hasNext() || parser.itemCount() == 0) break;
  }
  out.cached = writer.count();
  if (out.serverTotal < out.cached) out.serverTotal = out.cached;
  // The sort indices allocate per-book key arrays; give them the heap the
  // TLS session holds.
  client_.disconnect();
  if (client_.librarySelection().count > 1 && !client_.librarySelection().all && !writer.sortByTitle())
    return ClientError::FILE;
  if (beforeInstall) beforeInstall(beforeInstallCtx);
  if (!writer.commit(out.serverTotal, out.truncated, fetchedAt)) return ClientError::FILE;
  LOG_INF(TAG, "catalogue: %u cached of %lu%s", out.cached, static_cast<unsigned long>(out.serverTotal),
          out.truncated ? " (truncated)" : "");
  return ClientError::OK;
}

// --- covers ----------------------------------------------------------------------

int Session::deviceCoverW_ = 0;
int Session::deviceCoverH_ = 0;

void Session::setDeviceCoverSize(const int width, const int height) {
  deviceCoverW_ = width;
  deviceCoverH_ = height;
}

// Server thumbnails end at EOI. Reject interrupted legacy cache writes before
// reusing them; a checked HTTP download replaces the old file only on success.
static bool completeCachedJpeg(const std::string& path) {
  HalFile file;
  uint8_t marker[2];
  return Storage.openFileForRead(TAG, path, file) && file.fileSize() >= 4 &&
         file.read(marker, sizeof(marker)) == sizeof(marker) && marker[0] == 0xFF && marker[1] == 0xD8 &&
         file.seek(file.fileSize() - 2) && file.read(marker, sizeof(marker)) == sizeof(marker) && marker[0] == 0xFF &&
         marker[1] == 0xD9;
}

// Writes a finished list tile next to the retained assets (staged).
static bool saveTile(const std::string& tilePath, const uint8_t* tile) {
  const std::string tmp = tilePath + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite(TAG, tmp.c_str(), out) || out.write(tile, COVER_TILE_BYTES) != COVER_TILE_BYTES) {
    out.close();
    Storage.remove(tmp.c_str());
    return false;
  }
  if (!out.close()) return false;
  return storage::replace(tmp, tilePath);
}

void Session::refreshCovers() {
  CatalogCache cache;
  if (!cache.open(account_)) return;
  const std::string dirPath = coversDir(account_);
  Storage.mkdir(dirPath.c_str());

  uint16_t fetched = 0;
  uint32_t pendingIds[COVER_BATCH];
  uint16_t pending = 0;
  bool stop = false;
  // Server-rendered 1-bit covers (advertised as deviceThumbnails) are a few
  // KB and need no decoding. The answer's magic is still checked, so a
  // proxy or a downgrade cannot slip a JPEG into the BMP path.
  bool serverRendersCovers = client_.hasCapability(ServerCapability::DeviceThumbnails);

  auto flushBatch = [&]() {
    // Decode after the TLS connection is dropped; the next request reconnects.
    client_.disconnect();
    auto tile = makeUniqueNoThrow<uint8_t[]>(COVER_TILE_BYTES);
    for (uint16_t i = 0; i < pending; i++) {
      if (cancelled()) break;
      const std::string jpeg = coverTilePath(account_, pendingIds[i]) + ".jpg";
      const std::string tilePath = coverTilePath(account_, pendingIds[i]);
      bool ready = Storage.exists(tilePath.c_str());
      if (!ready && tile && renderCoverTile(jpeg, tile.get())) ready = saveTile(tilePath, tile.get());
      if (deviceCoverH_ > 0) {
        const std::string thumb = coverThumbPath(account_, pendingIds[i], deviceCoverH_);
        if (!Storage.exists(thumb.c_str())) {
          const std::string tmp = thumb + ".tmp";
          HalFile in, out;
          bool saved = Storage.openFileForRead(TAG, jpeg, in) && Storage.openFileForWrite(TAG, tmp, out) &&
                       JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(in, out, deviceCoverW_, deviceCoverH_);
          if (!out.close()) saved = false;
          in.close();
          if (saved) saved = storage::replace(tmp, thumb);
          if (!saved) Storage.remove(tmp.c_str());
          ready = ready && saved;
        }
      }
      if (ready)
        ++fetched;
      else
        LOG_ERR(TAG, "cover %lu could not be prepared; retained JPEG for retry",
                static_cast<unsigned long>(pendingIds[i]));
    }
    pending = 0;
  };

  // Rendered path: the list tile streamed straight into its sink, then the
  // grid thumbnail at the device's cover size saved as-is. Returns false when
  // the server turned out not to render covers (the JPEG path takes over).
  auto tileBuf = makeUniqueNoThrow<uint8_t[]>(COVER_TILE_BYTES);
  auto fetchRendered = [&](const uint32_t bookId, const std::string& tilePath, bool& got) -> ClientError {
    got = false;
    if (!Storage.exists(tilePath.c_str())) {
      if (!tileBuf) return ClientError::LOW_MEMORY;
      memset(tileBuf.get(), 0, COVER_TILE_BYTES);
      CoverTileSink sink(tileBuf.get());
      const ClientError err = client_.downloadCoverBmp(bookId, COVER_W, COVER_H, &sink, nullptr);
      if (err != ClientError::OK) return err;
      if (!sink.complete()) return ClientError::JSON;
      if (!saveTile(tilePath, tileBuf.get())) return ClientError::FILE;
      got = true;
    }
    if (deviceCoverH_ > 0) {
      const std::string thumb = coverThumbPath(account_, bookId, deviceCoverH_);
      if (!Storage.exists(thumb.c_str())) {
        const ClientError err = client_.downloadCoverBmp(bookId, deviceCoverW_, deviceCoverH_, nullptr, &thumb);
        if (err != ClientError::OK) return err;
        got = true;
      }
    }
    return ClientError::OK;
  };

  CatalogRecord record;
  uint16_t missing = 0;
  phase(SessionPhase::Covers);
  for (uint16_t ordinal = 0; ordinal < cache.count(); ++ordinal) {
    if (cancelled()) return;
    if (!cache.readRecord(ordinal, record) || !record.hasCover()) continue;
    if (!Storage.exists(coverTilePath(account_, record.bookId).c_str()) ||
        (deviceCoverH_ > 0 && !Storage.exists(coverThumbPath(account_, record.bookId, deviceCoverH_).c_str())))
      ++missing;
  }
  phase(SessionPhase::Covers, 0, missing);
  // Two passes over the catalogue: managed books, then everything else.
  for (int pass = 0; pass < 2 && !stop; pass++) {
    for (uint16_t ordinal = 0; ordinal < cache.count(); ordinal++) {
      if (!cache.readRecord(ordinal, record) || !record.hasCover()) continue;
      const bool managed = files_.indexOfBook(record.bookId) >= 0;
      if ((pass == 0) != managed) continue;
      const std::string tilePath = coverTilePath(account_, record.bookId);
      const bool needTile = !Storage.exists(tilePath.c_str());
      const bool needThumb =
          deviceCoverH_ > 0 && !Storage.exists(coverThumbPath(account_, record.bookId, deviceCoverH_).c_str());
      if (!needTile && !needThumb) continue;
      // One e-ink refresh per cover would dominate the phase; report per batch.
      if (pending == 0 && fetched % COVER_BATCH == 0) phase(SessionPhase::Covers, fetched, missing);
      if (cancelled()) {
        stop = true;
        break;
      }
      ClientError err;
      if (serverRendersCovers) {
        bool got = false;
        err = fetchRendered(record.bookId, tilePath, got);
        if (err == ClientError::NOT_SUPPORTED) {
          LOG_ERR(TAG, "server advertised device thumbnails but served a JPEG; using JPEG thumbnails");
          serverRendersCovers = false;
          client_.disconnect();  // the dropped transfer left the connection unusable
          const auto jpeg = tilePath + ".jpg";
          err = completeCachedJpeg(jpeg) ? ClientError::OK : client_.downloadThumbnail(record.bookId, jpeg);
          if (err == ClientError::OK) pendingIds[pending++] = record.bookId;
        } else if (err == ClientError::OK && !got) {
          continue;
        }
      } else {
        const auto jpeg = tilePath + ".jpg";
        err = completeCachedJpeg(jpeg) ? ClientError::OK : client_.downloadThumbnail(record.bookId, jpeg);
        if (err == ClientError::OK) pendingIds[pending++] = record.bookId;
      }
      if (err == ClientError::NOT_FOUND) continue;  // advertised cover missing on disk
      if (err != ClientError::OK) {
        LOG_ERR(TAG, "cover %lu: %s", static_cast<unsigned long>(record.bookId), clientErrorName(err));
        if (err == ClientError::AUTH || err == ClientError::FORBIDDEN || err == ClientError::NETWORK ||
            err == ClientError::TLS_FAILED || err == ClientError::LOW_MEMORY) {
          stop = true;  // no point retrying every cover
          break;
        }
        continue;
      }
      if (serverRendersCovers) fetched++;
      if (pending == COVER_BATCH) flushBatch();
    }
  }
  if (pending > 0) flushBatch();
  phase(SessionPhase::Covers, fetched, missing);

  // Eviction: only when the directory outgrew the catalogue, since it needs
  // a resident id table (4 bytes per cached book).
  HalFile dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) return;
  uint16_t entries = 0;
  char name[40];
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    entry.close();
    // .tmp leftovers only exist after an interrupted session.
    const size_t len = strlen(name);
    if (len > 4 && strcmp(name + len - 4, ".tmp") == 0) {
      Storage.remove((dirPath + "/" + name).c_str());
      continue;
    }
    entries++;
  }
  dir.rewindDirectory();
  if (entries <= cache.count() + 64) {
    dir.close();
    return;
  }
  auto ids = makeUniqueNoThrow<uint32_t[]>(cache.count() > 0 ? cache.count() : 1);
  if (!ids) {
    dir.close();
    return;
  }
  for (uint16_t ordinal = 0; ordinal < cache.count(); ordinal++) {
    ids[ordinal] = cache.readRecord(ordinal, record) ? record.bookId : 0;
  }
  std::sort(ids.get(), ids.get() + cache.count());
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    entry.close();
    const uint32_t bookId = static_cast<uint32_t>(strtoul(name, nullptr, 10));
    const bool live = bookId != 0 && std::binary_search(ids.get(), ids.get() + cache.count(), bookId);
    if (!live) Storage.remove((dirPath + "/" + name).c_str());
  }
  dir.close();
}

// --- insights ------------------------------------------------------------------

namespace {
// Current month from the wall clock (UTC), or false when the clock is unset.
bool currentMonth(uint16_t& year, uint8_t& month) {
  const uint32_t now = nowIfClockValid();
  if (now == 0) return false;
  const time_t t = static_cast<time_t>(now);
  struct tm parts{};
  gmtime_r(&t, &parts);
  year = static_cast<uint16_t>(parts.tm_year + 1900);
  month = static_cast<uint8_t>(parts.tm_mon + 1);
  return true;
}

template <typename Parser, typename Record>
bool bodyIsValid(const std::string& body) {
  auto record = makeUniqueNoThrow<Record>();
  if (!record) return false;
  auto parser = makeUniqueNoThrow<Parser>(*record);
  if (!parser) return false;
  parser->feed(body.data(), body.size());
  return parser->finish();
}

void snapshotHeader(JsonDocument& doc) {
  doc["schemaVersion"] = INSIGHTS_SCHEMA_VERSION;
  const time_t now = nowIfClockValid();
  if (now) {
    struct tm parts{};
    gmtime_r(&now, &parts);
    char iso[21];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &parts);
    doc["generatedAt"] = iso;
  }
}

ClientError fetchHeatmap(Client& client, uint16_t days, InsightsCalendar& calendar,
                         InsightsSummary* summary = nullptr) {
  // Parser and display records exceed the embedded task's stack budget.
  auto parser = makeUniqueNoThrow<InsightsHeatmapParser>(calendar, summary);
  if (!parser) return ClientError::LOW_MEMORY;
  char query[72];
  snprintf(query, sizeof(query), "/koreader/plugin/insights/reading-heatmap?days=%u", days);
  auto error = client.getStream(
      query,
      [](void* context, const char* data, size_t size) {
        static_cast<InsightsHeatmapParser*>(context)->feed(data, size);
        return true;
      },
      parser.get());
  if (error != ClientError::OK) return error;
  return parser->finish(days) ? ClientError::OK : ClientError::JSON;
}
}  // namespace

ClientError Session::fetchInsightsMonth(const uint16_t year, const uint8_t month) {
  if (!client_.hasCapability(ServerCapability::ReadingInsights)) return ClientError::NOT_FOUND;
  const uint16_t days = insightsHeatmapDays(year, month, nowIfClockValid());
  if (!days) return ClientError::JSON;
  auto calendar = makeUniqueNoThrow<InsightsCalendar>();
  if (!calendar) return ClientError::LOW_MEMORY;
  calendar->year = year;
  calendar->month = month;
  const auto error = fetchHeatmap(client_, days, *calendar);
  if (error != ClientError::OK) return error;
  if (!calendar->valid) return ClientError::JSON;
  std::string body;
  serializeInsightsCalendar(*calendar, body);
  if (!bodyIsValid<InsightsCalendarParser, InsightsCalendar>(body)) return ClientError::JSON;
  return saveInsightsBody(insightsCalendarPath(account_, year, month), body) ? ClientError::OK : ClientError::FILE;
}

void Session::noteInsightsResult(SyncState& state, const ClientError error, const uint32_t at) {
  if (error == ClientError::OK) {
    state.insightsAt = at;
    state.insightsUnsupported = false;
    state.insightsDisabled = false;
  } else if (error == ClientError::NOT_FOUND) {
    state.insightsUnsupported = true;
  } else if (error == ClientError::FORBIDDEN) {
    state.insightsDisabled = true;
  }
}

ClientError Session::refreshInsights(uint16_t year, uint8_t month) {
  // Not advertised: the routes are absent (an older BookOrbit); reported as
  // NOT_FOUND so the screens say so, without a request that would 404.
  if (!client_.hasCapability(ServerCapability::ReadingInsights)) return ClientError::NOT_FOUND;
  JsonDocument snapshot;
  snapshotHeader(snapshot);
  snapshot["daysTimezone"] = "UTC";
  static constexpr const char* routes[] = {"reading-goal", "reading-streak", "reading-dna", "year-projection",
                                           "summary"};
  static constexpr const char* sections[] = {"goal", "streak", "dna", "projection", "library"};
  std::string body;
  for (unsigned i = 0; i < 5; ++i) {
    if (cancelled()) return ClientError::ABORTED;
    phase(SessionPhase::Insights, i, 9);
    auto error = client_.fetchJson(std::string("/koreader/plugin/insights/") + routes[i], body);
    if (error != ClientError::OK) return error;
    if (!addInsightsWidget(snapshot, sections[i], body)) return ClientError::JSON;
  }
  {
    auto summary = makeUniqueNoThrow<InsightsSummary>();
    auto calendar = makeUniqueNoThrow<InsightsCalendar>();
    if (!summary || !calendar) return ClientError::LOW_MEMORY;
    if (cancelled()) return ClientError::ABORTED;
    phase(SessionPhase::Insights, 5, 9);
    auto error = fetchHeatmap(client_, 30, *calendar, summary.get());
    if (error != ClientError::OK) return error;
    addInsightsHeatmap(snapshot, *summary);
  }
  body.clear();
  if (snapshot.overflowed()) return ClientError::LOW_MEMORY;
  serializeJson(snapshot, body);
  snapshot.clear();
  if (!bodyIsValid<InsightsSummaryParser, InsightsSummary>(body)) return ClientError::JSON;
  if (!saveInsightsBody(insightsSummaryPath(account_), body)) return ClientError::FILE;

  snapshotHeader(snapshot);
  snapshot["days"] = 90;
  snapshot["weekdaysTimezone"] = "UTC";
  for (unsigned i = 0; i < 2; ++i) {
    if (cancelled()) return ClientError::ABORTED;
    phase(SessionPhase::Insights, 6 + i, 9);
    auto error = client_.fetchJson(
        i ? "/koreader/plugin/insights/peak-hours?days=90" : "/koreader/plugin/insights/favorite-days?days=90", body);
    if (error != ClientError::OK) return error;
    if (!addInsightsBuckets(snapshot, i != 0, body)) return ClientError::JSON;
  }
  body.clear();
  if (snapshot.overflowed()) return ClientError::LOW_MEMORY;
  serializeJson(snapshot, body);
  snapshot.clear();
  if (!bodyIsValid<InsightsHabitsParser, InsightsHabits>(body)) return ClientError::JSON;
  if (!saveInsightsBody(insightsHabitsPath(account_), body)) return ClientError::FILE;
  if (cancelled()) return ClientError::ABORTED;
  phase(SessionPhase::Insights, 8, 9);
  if ((year == 0 || month == 0) && !currentMonth(year, month)) return ClientError::OK;
  // Historical requests use a trailing day window, then retain only this month.
  body.clear();
  body.shrink_to_fit();
  const auto error = fetchInsightsMonth(year, month);
  if (error != ClientError::OK) return error;
  pruneInsightsCalendars(account_, INSIGHTS_KEEP_MONTHS);
  phase(SessionPhase::Insights, 9, 9);
  return ClientError::OK;
}

// --- progress -----------------------------------------------------------------

ProgressStats Session::syncProgress(const ReaderSnapshot& openBook, const bool askOnConflict,
                                    std::vector<ProgressConflict>& conflicts) {
  ProgressStats stats;
  if (!ReadingQueue::pruneSynced(nowIfClockValid())) LOG_ERR("BO", "Reading archive cleanup failed");
  if (!client_.librarySelection().all && !client_.librarySelection().count) return stats;
  phase(SessionPhase::Matching);
  const auto matching = matchSideloads();
  if (matching != ClientError::OK && matching != ClientError::NOT_FOUND) ++stats.errors;
  ManagedFile file;
  for (uint16_t i = 0; i < files_.count(); i++) {
    phase(SessionPhase::Progress, i, files_.count());
    if (cancelled()) return stats;
    if (!files_.read(i, file) || !file.complete()) continue;
    bool selected = false;
    const auto selectionError = client_.checkBookSelection(file.bookId, selected);
    if (selectionError != ClientError::OK) {
      ++stats.errors;
      continue;
    }
    if (!selected) continue;
    const std::string path = files_.pathFor(file);
    if (!Storage.exists(path.c_str())) continue;
    if ((file.flags & FILE_FLAG_SIDELOAD) && KOReaderDocumentId::calculate(path) != file.fileHash) {
      ++stats.errors;  // The bytes changed: never send old identities for this path.
      continue;
    }
    ClientError error = ClientError::OK;
    if (!syncOneFile(file, path, openBook, askOnConflict, conflicts, stats, error)) {
      stats.errors++;
      LOG_ERR(TAG, "progress for book %lu: %s", static_cast<unsigned long>(file.bookId), clientErrorName(error));
      if (error == ClientError::AUTH || error == ClientError::FORBIDDEN) return stats;
    }
    if (openBook.path != path && validDocumentHash(file.fileHash)) {
      for (auto kind : {AnnotationKind::Bookmark, AnnotationKind::Highlight}) {
        error = syncAnnotations(client_, account_, path, file.fileHash, kind, callbacks_.shouldCancel, callbacks_.ctx);
        if (error != ClientError::OK && error != ClientError::NOT_FOUND) ++stats.errors;
        if (cancelled()) return stats;
      }
    }
    error = syncReadingEvents(file, path);
    if (error != ClientError::OK && error != ClientError::NOT_FOUND) {
      ++stats.errors;
      LOG_ERR(TAG, "reading events for book %lu: %s", static_cast<unsigned long>(file.bookId), clientErrorName(error));
    }
  }
  phase(SessionPhase::Progress, files_.count(), files_.count());
  return stats;
}

ClientError Session::matchSideloads() {
  auto index = makeUniqueNoThrow<library::LibraryIndexFile>();
  if (!index) return ClientError::LOW_MEMORY;
  if (!index->open(library::libraryIndexPath()) || index->bookCount() == 0) return ClientError::OK;
  const std::string cursorPath = accountDir(account_) + "/match-cursor.json";
  JsonDocument cursor;
  PersistableStoreBase::readDocFromFile(cursorPath.c_str(), cursor);
  unsigned ordinal = (cursor["next"] | 0u) % index->bookCount();
  for (unsigned checked = 0; checked < 16 && checked < index->bookCount() && !cancelled(); ++checked) {
    library::ClixRecord record;
    std::string path;
    if (!index->readRecord(ordinal, record) || !index->readPath(record, path)) return ClientError::FILE;
    ordinal = (ordinal + 1) % index->bookCount();
    if (isManagedPath(path) || path.size() < 5 || strcasecmp(path.c_str() + path.size() - 5, ".epub") != 0) continue;
    const std::string hash = KOReaderDocumentId::calculate(path);
    if (!validDocumentHash(hash)) continue;
    JsonDocument doc;
    deviceEnvelope(doc, BOOKORBIT_STORE.getDeviceId(), "CrossPoint");
    doc["hashes"].to<JsonArray>().add(hash);
    std::string request, response;
    serializeJson(doc, request);
    doc.clear();
    const auto error = client_.postJson("/koreader/plugin/match-check", request, response);
    if (error != ClientError::OK) return error;
    if (deserializeJson(doc, response) || !doc["matches"].is<JsonArray>()) return ClientError::JSON;
    for (JsonObjectConst match : doc["matches"].as<JsonArrayConst>()) {
      if (hash != (match["hash"] | "")) continue;
      const uint32_t bookId = match["bookId"] | 0u;
      const uint32_t fileId = match["bookFileId"] | 0u;
      if (bookId == 0 || fileId == 0 || files_.isComplete(bookId)) continue;
      bool selected = false;
      const auto selectionError = client_.checkBookSelection(bookId, selected);
      if (selectionError != ClientError::OK) return selectionError;
      if (!selected) continue;
      ManagedFile file;
      if (files_.find(bookId, file) && (file.flags & FILE_FLAG_SIDELOAD)) continue;
      file = ManagedFile{};
      file.bookId = bookId;
      file.fileId = fileId;
      memcpy(file.fileHash, hash.c_str(), hash.size() + 1);
      if (!files_.attachSideload(file, path)) return ClientError::FILE;
    }
  }
  cursor["next"] = ordinal;
  return PersistableStoreBase::writeDocToFile(cursorPath.c_str(), cursor) ? ClientError::OK : ClientError::FILE;
}

ClientError Session::syncReadingEvents(const ManagedFile& file, const std::string& path) {
  if (!validDocumentHash(file.fileHash)) return ClientError::OK;
  ReadingQueue queue(path);
  // A single reusable batch keeps the task stack below the full queue's size.
  auto events = makeUniqueNoThrow<ReadingEvent[]>(ReadingQueue::BATCH);
  if (!events) return ClientError::LOW_MEMORY;
  unsigned uploaded = 0;
  for (unsigned batch = 0; batch < 8 && !cancelled(); ++batch) {
    unsigned count = 0, queued = 0;
    if (!queue.read(events.get(), ReadingQueue::BATCH, count, &queued)) return ClientError::FILE;
    if (queued || uploaded) phase(SessionPhase::ReadingEvents, uploaded, queued);
    if (count == 0) return ClientError::OK;
    JsonDocument doc;
    deviceEnvelope(doc, BOOKORBIT_STORE.getDeviceId(), "CrossPoint");
    JsonObject book = doc["books"].to<JsonArray>().add<JsonObject>();
    book["hash"] = file.fileHash;
    JsonArray list = book["events"].to<JsonArray>();
    unsigned ready = 0;
    for (; ready < count; ++ready) {
      if (!ReadingTracker::resolveTime(events[ready])) break;
      const auto& stat = events[ready].stat;
      JsonObject event = list.add<JsonObject>();
      event["page"] = stat.page;
      event["startTime"] = stat.startTime;
      event["durationSeconds"] = stat.durationSeconds;
      event["totalPages"] = stat.totalPages;
    }
    if (ready == 0) {
      if (events[0].boot == ReadingTracker::bootId()) return ClientError::OK;
      if (!queue.preserveUndated(events[0]) || !queue.acknowledge(1)) return ClientError::FILE;
      continue;
    }
    if (doc.overflowed()) return ClientError::LOW_MEMORY;
    std::string request, response;
    serializeJson(doc, request);
    doc.clear();
    const auto error = client_.postJson("/koreader/plugin/page-stats", request, response);
    if (error != ClientError::OK) return error;
    if (!statsAccepted(response, file.fileHash, ready)) return ClientError::JSON;
    if (!queue.acknowledgeSynced(events.get(), ready, nowIfClockValid())) return ClientError::FILE;
    uploaded += ready;
    phase(SessionPhase::ReadingEvents, uploaded, queued - ready);
  }
  return cancelled() ? ClientError::ABORTED : ClientError::OK;
}

bool Session::syncOneFile(ManagedFile& file, const std::string& path, const ReaderSnapshot& openBook,
                          const bool askOnConflict, std::vector<ProgressConflict>& conflicts, ProgressStats& stats,
                          ClientError& error) {
  // The manifest hash is the server's identity for these exact bytes; without
  // one, the same partial-MD5 computed locally is the fallback.
  std::string hash = file.fileHash;
  if (hash.empty()) {
    hash = KOReaderDocumentId::calculate(path);
    if (hash.empty()) {
      error = ClientError::FILE;
      return false;
    }
    strncpy(file.fileHash, hash.c_str(), FILE_HASH_BYTES);
    file.fileHash[FILE_HASH_BYTES] = '\0';
    if (!files_.upsert(file)) {
      error = ClientError::FILE;
      return false;
    }
  }

  if (!openBook.valid && openBook.path == path) {
    error = ClientError::ABORTED;
    return false;
  }
  const bool isOpen = openBook.valid && openBook.path == path;
  LocalProgressFile local;
  if (isOpen) {
    local = openBook.local;
  } else if (!readLocalProgress(path, local)) {
    error = ClientError::FILE;
    return false;
  }
  RemoteProgress remote;
  error = client_.getProgress(hash, remote);
  if (error != ClientError::OK) return false;
  if (cancelled()) {
    error = ClientError::ABORTED;
    return false;
  }

  RemoteProgressState remoteState;
  remoteState.found = remote.found;
  remoteState.fromThisDevice = remote.found && remote.deviceId == BOOKORBIT_STORE.getDeviceId();
  remoteState.timestamp = remote.timestamp;
  remoteState.hash = locatorHash(remote.locator, remote.percentage);
  AckState ack;
  ack.hasLocalAck = file.hasLocalAck();
  ack.ackSpine = file.ackLocalSpine;
  ack.ackKey = file.ackLocalKey;
  ack.hasRemoteAck = file.hasRemoteAck();
  ack.ackTimestamp = file.ackRemoteTs;
  ack.ackHash = file.ackRemoteHash;

  const auto decision = decideSync(local.toState(), remoteState, ack);
  LOG_DBG(TAG, "book %lu: local=%d remote=%d ours=%d open=%d -> %d", static_cast<unsigned long>(file.bookId),
          local.valid, remote.found, remoteState.fromThisDevice, isOpen, static_cast<int>(decision));

  switch (decision) {
    case SyncDecision::NothingToDo:
      if (remote.found && !file.hasRemoteAck()) {
        file.flags |= FILE_FLAG_ACK_REMOTE;
        file.ackRemoteTs = remote.timestamp;
        file.ackRemoteHash = remoteState.hash;
        if (!files_.upsert(file)) {
          error = ClientError::FILE;
          return false;
        }
      }
      return true;
    case SyncDecision::Upload: {
      std::string locator;
      float pct = 0;
      if (isOpen) {
        locator = openBook.locator;
        pct = openBook.percentage;
      } else {
        client_.disconnect();  // free the TLS session before chapter streaming
        if (!buildLocalLocator(path, local, renderer_, locator, pct, lendFramebuffer_)) {
          error = ClientError::FILE;
          return false;
        }
      }
      uint32_t ackTs = 0;
      error = client_.putProgress(hash, locator, pct, nowIfClockValid(), ackTs);
      if (error != ClientError::OK) return false;
      acknowledge(file, local, ackTs, locatorHash(locator, pct));
      if (!files_.upsert(file)) {
        error = ClientError::FILE;
        return false;
      }
      stats.uploaded++;
      return true;
    }
    case SyncDecision::ApplyRemote: {
      // A displayed page is never repositioned under the reader; the newer
      // server position is picked up when the book is next opened.
      if (isOpen) return true;
      client_.disconnect();
      LocalProgressFile written;
      if (!applyRemoteLocator(path, remote.locator, remote.percentage, renderer_, written, lendFramebuffer_)) {
        error = ClientError::FILE;
        return false;
      }
      acknowledge(file, written, remote.timestamp, remoteState.hash);
      if (!files_.upsert(file)) {
        error = ClientError::FILE;
        return false;
      }
      stats.applied++;
      return true;
    }
    case SyncDecision::Conflict: {
      stats.conflicts++;
      // Persist before showing a choice: Back/cancellation must leave the
      // unresolved conflict visible even when it was first found interactively.
      if (!file.hasConflict()) {
        file.flags |= FILE_FLAG_CONFLICT;
        if (!files_.upsert(file)) {
          error = ClientError::FILE;
          return false;
        }
      }
      if (!askOnConflict || conflicts.size() >= MAX_CONFLICTS) return true;
      ProgressConflict conflict;
      conflict.bookId = file.bookId;
      conflict.documentHash = hash;
      conflict.local = local;
      conflict.remote = remote;
      if (isOpen) {
        conflict.localLocator = openBook.locator;
        conflict.localPct = openBook.percentage;
      } else {
        client_.disconnect();
        if (!buildLocalLocator(path, local, renderer_, conflict.localLocator, conflict.localPct, lendFramebuffer_)) {
          error = ClientError::FILE;
          return false;
        }
      }
      conflicts.push_back(std::move(conflict));
      return true;
    }
  }
  return true;
}

ClientError Session::loadConflict(const uint32_t bookId, ProgressConflict& out, bool& found) {
  found = false;
  bool selected = false;
  const auto selectionError = client_.checkBookSelection(bookId, selected);
  if (selectionError != ClientError::OK) return selectionError;
  if (!selected) return ClientError::LIBRARY_NOT_SELECTED;
  if (cancelled()) return ClientError::ABORTED;
  ManagedFile file;
  if (!files_.find(bookId, file) || !file.complete()) return ClientError::NOT_FOUND;
  const std::string path = files_.pathFor(file);
  if (path.empty() || !Storage.exists(path.c_str())) return ClientError::FILE;
  if ((file.flags & FILE_FLAG_SIDELOAD) && KOReaderDocumentId::calculate(path) != file.fileHash)
    return ClientError::FILE;
  std::vector<ProgressConflict> conflicts;
  conflicts.reserve(1);
  ProgressStats stats;
  ClientError error = ClientError::OK;
  if (!syncOneFile(file, path, ReaderSnapshot{}, /*askOnConflict=*/true, conflicts, stats, error)) return error;
  if (conflicts.empty()) {
    // Settled meanwhile (uploaded, applied, or nothing to do): drop the flag.
    if (file.hasConflict()) {
      file.flags &= static_cast<uint8_t>(~FILE_FLAG_CONFLICT);
      if (!files_.upsert(file)) return ClientError::FILE;
    }
    return ClientError::OK;
  }
  out = std::move(conflicts.front());
  found = true;
  return ClientError::OK;
}

bool Session::resolveConflict(const ProgressConflict& conflict, const bool keepLocal) {
  bool selected = false;
  if (client_.checkBookSelection(conflict.bookId, selected) != ClientError::OK || !selected) return false;
  ManagedFile file;
  if (!files_.find(conflict.bookId, file)) return false;
  const std::string path = files_.pathFor(file);
  if (keepLocal) {
    uint32_t ackTs = 0;
    const auto error =
        client_.putProgress(conflict.documentHash, conflict.localLocator, conflict.localPct, nowIfClockValid(), ackTs);
    if (error != ClientError::OK) return false;
    acknowledge(file, conflict.local, ackTs, locatorHash(conflict.localLocator, conflict.localPct));
    return files_.upsert(file);
  }
  client_.disconnect();
  LocalProgressFile written;
  if (!applyRemoteLocator(path, conflict.remote.locator, conflict.remote.percentage, renderer_, written,
                          lendFramebuffer_)) {
    return false;
  }
  acknowledge(file, written, conflict.remote.timestamp,
              locatorHash(conflict.remote.locator, conflict.remote.percentage));
  return files_.upsert(file);
}

// --- download ---------------------------------------------------------------------

bool Session::onDownloadProgress(void* ctx, const size_t received, const size_t total) {
  auto* self = static_cast<Session*>(ctx);
  self->phase(SessionPhase::Download, static_cast<uint32_t>(received), static_cast<uint32_t>(total));
  if (self->cancelled()) self->cancelFlag_ = true;
  return !self->cancelFlag_;
}

ClientError Session::downloadBook(const uint32_t bookId, ManagedFile& record, std::string& finalPath) {
  bool selected = false;
  const auto selectionError = client_.checkBookSelection(bookId, selected);
  if (selectionError != ClientError::OK) return selectionError;
  if (!selected) return ClientError::LIBRARY_NOT_SELECTED;
  if (files_.count() >= FILE_STORE_MAX && files_.indexOfBook(bookId) < 0) return ClientError::FILE;
  phase(SessionPhase::Download, 0, 0);

  FileManifest manifest;
  ClientError error = client_.fetchManifest(bookId, manifest);
  if (error != ClientError::OK) return error;

  // An identical version already on the card needs no transfer.
  ManagedFile existing;
  if (files_.find(bookId, existing) && existing.complete() && existing.fileId == manifest.fileId &&
      strcmp(existing.contentVersion, manifest.contentVersion) == 0 &&
      Storage.exists(files_.pathFor(existing).c_str())) {
    record = existing;
    finalPath = files_.pathFor(existing);
    return ClientError::OK;
  }

  // Space for the staging copy plus headroom; the old complete copy (if any)
  // stays until the new one is committed, so it is not counted as reclaimable.
  const uint64_t total = Storage.totalBytes();
  if (total > 0) {
    const uint64_t used = Storage.usedBytes();
    const uint64_t free = total > used ? total - used : 0;
    if (free < static_cast<uint64_t>(manifest.sizeBytes) + FREE_SPACE_MARGIN) {
      LOG_ERR(TAG, "free %llu bytes, need %lu", static_cast<unsigned long long>(free),
              static_cast<unsigned long>(manifest.sizeBytes));
      return ClientError::FILE;
    }
  }

  Storage.mkdir(transfersDir(account_).c_str());
  const std::string stagingPath = stagingFilePath(account_, manifest.fileId);
  cancelFlag_ = false;
  LOG_INF(TAG, "downloading book %lu file %lu (%lu bytes, v%s)", static_cast<unsigned long>(bookId),
          static_cast<unsigned long>(manifest.fileId), static_cast<unsigned long>(manifest.sizeBytes),
          manifest.contentVersion);
  error = client_.downloadFile(manifest.fileId, stagingPath, manifest.sizeBytes, &Session::onDownloadProgress, this,
                               &cancelFlag_);
  if (error != ClientError::OK) return error;

  phase(SessionPhase::Verify);
  std::string hash;
  if (!verifyStagedFile(stagingPath, manifest, hash)) {
    Storage.remove(stagingPath.c_str());
    return ClientError::SIZE_MISMATCH;
  }
  phase(SessionPhase::Publish);
  if (!publish(stagingPath, manifest, hash, record, finalPath)) {
    Storage.remove(stagingPath.c_str());
    return ClientError::FILE;
  }
  return ClientError::OK;
}

// Structural check (ZIP local-header magic: an EPUB is an OCF zip container),
// exact byte count against the manifest, and the partial MD5 the server
// keyed the file by. A missing server hash is recorded from the bytes so the
// progress exchange has an identity, but it does not count as a match.
bool Session::verifyStagedFile(const std::string& stagingPath, const FileManifest& manifest,
                               std::string& computedHash) {
  HalFile file;
  if (!Storage.openFileForRead(TAG, stagingPath.c_str(), file)) return false;
  uint8_t magic[4];
  if (file.read(magic, sizeof(magic)) != static_cast<int>(sizeof(magic)) || magic[0] != 'P' || magic[1] != 'K' ||
      magic[2] != 3 || magic[3] != 4) {
    LOG_ERR(TAG, "staged file is not a zip container");
    return false;
  }
  const size_t size = file.fileSize();
  file.close();
  if (manifest.sizeBytes > 0 && size != manifest.sizeBytes) {
    LOG_ERR(TAG, "staged size %u != manifest %lu", static_cast<unsigned>(size),
            static_cast<unsigned long>(manifest.sizeBytes));
    return false;
  }
  computedHash = KOReaderDocumentId::calculate(stagingPath);
  if (computedHash.empty()) return false;
  if (manifest.fileHash[0] != '\0' && !equalsIgnoreCase(computedHash.c_str(), manifest.fileHash)) {
    LOG_ERR(TAG, "hash mismatch: %s vs manifest %s", computedHash.c_str(), manifest.fileHash);
    return false;
  }
  return true;
}

// Moves the verified bytes to the managed path, then records the mapping. The
// previous version's file is deleted only after the new record is durable.
bool Session::publish(const std::string& stagingPath, const FileManifest& manifest, const std::string& hash,
                      ManagedFile& record, std::string& finalPath) {
  Storage.mkdir(filesDir(account_).c_str());
  finalPath = managedFilePath(account_, manifest.bookId, manifest.fileId);

  ManagedFile previous;
  const bool hadPrevious = files_.find(manifest.bookId, previous);
  const std::string previousPath = hadPrevious ? files_.pathFor(previous) : "";
  // The reader cache (and the saved position in it) is keyed by path. It is
  // kept only when the bytes are provably the ones it was built from: same
  // file, same version, same hash as the record left by an earlier download.
  const bool sameBytes = hadPrevious && previous.fileId == manifest.fileId &&
                         strcmp(previous.contentVersion, manifest.contentVersion) == 0 &&
                         equalsIgnoreCase(previous.fileHash, hash.c_str());

  if (!storage::replace(stagingPath, finalPath)) {
    LOG_ERR(TAG, "cannot move staged file into place");
    return false;
  }
  if (!sameBytes) clearBookCache(finalPath);

  record = sameBytes ? previous : ManagedFile{};
  record.bookId = manifest.bookId;
  record.fileId = manifest.fileId;
  record.sizeBytes = manifest.sizeBytes;
  record.downloadedAt = nowIfClockValid();
  record.flags |= FILE_FLAG_COMPLETE;
  record.flags &= static_cast<uint8_t>(~FILE_FLAG_SIDELOAD);
  strncpy(record.contentVersion, manifest.contentVersion, FILE_VERSION_BYTES);
  strncpy(record.fileHash, hash.c_str(), FILE_HASH_BYTES);
  if (!files_.upsert(record)) {
    LOG_ERR(TAG, "cannot record managed file");
    return false;
  }
  if (hadPrevious && !(previous.flags & FILE_FLAG_SIDELOAD) && previousPath != finalPath &&
      Storage.exists(previousPath.c_str())) {
    clearBookCache(previousPath);
    Storage.remove(previousPath.c_str());
  }
  LOG_INF(TAG, "published %s", finalPath.c_str());
  return true;
}

void Session::checkIn(ManagedFile& record, const std::string& finalPath) {
  bool selected = false;
  if (client_.checkBookSelection(record.bookId, selected) != ClientError::OK || !selected) return;
  RemoteProgress remote;
  if (client_.getProgress(record.fileHash, remote) != ClientError::OK) return;
  std::string locator;
  float pct = 0;
  LocalProgressFile local;
  if (remote.found) {
    client_.disconnect();
    if (!applyRemoteLocator(finalPath, remote.locator, remote.percentage, renderer_, local, lendFramebuffer_)) return;
    locator = remote.locator;
    pct = remote.percentage;
  } else {
    // Nothing anywhere yet: a book just added to the device starts at 0%.
    // The echo below is what lists this device in BookOrbit.
    local.valid = true;
  }
  uint32_t ackTs = 0;
  if (client_.putProgress(record.fileHash, locator, pct, nowIfClockValid(), ackTs) != ClientError::OK) return;
  acknowledge(record, local, ackTs, locatorHash(locator, pct));
  files_.upsert(record);
}

}  // namespace bookorbit
