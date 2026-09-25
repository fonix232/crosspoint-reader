#pragma once

#include <GfxRenderer.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "BookOrbitClient.h"
#include "BookOrbitFileStore.h"
#include "BookOrbitProgressSync.h"
#include "BookOrbitSyncState.h"

namespace bookorbit {

// The work of one connected BookOrbit session, free of any screen: catalogue
// refresh, cover tiles, progress exchange, and download-and-publish. The
// foreground activities and the background agent both drive it; they differ
// only in how they report phases and what they do with a conflict.
//
// Every step polls `shouldCancel` between network calls and the client
// itself between body chunks, so a cancel lands within one transfer.

enum class SessionPhase : uint8_t {
  Idle,
  Connecting,
  Catalog,
  Covers,
  Progress,
  Download,
  Verify,
  Publish,
  Insights,
  Matching,
  ReadingEvents
};

struct SessionCallbacks {
  void* ctx = nullptr;
  // Phase plus a pair of numbers whose meaning depends on the phase
  // (completed/total items or bytes; ReadingEvents is uploaded/still queued).
  // Callbacks run only while the framebuffer is not borrowed by a mapper.
  void (*onPhase)(void* ctx, SessionPhase phase, uint32_t a, uint32_t b) = nullptr;
  bool (*shouldCancel)(void* ctx) = nullptr;
};

// A book whose position moved on both sides since the last agreement.
struct ProgressConflict {
  uint32_t bookId = 0;
  std::string documentHash;
  LocalProgressFile local;
  std::string localLocator;
  float localPct = 0;
  RemoteProgress remote;
};

// The reader's live position for the book it has open, handed to a session by
// the reader task so the session never touches an open book's caches.
struct ReaderSnapshot {
  bool valid = false;
  std::string path;
  LocalProgressFile local;
  std::string locator;
  float percentage = 0;
};

struct ProgressStats {
  uint16_t uploaded = 0;
  uint16_t applied = 0;
  uint16_t errors = 0;
  uint16_t conflicts = 0;
};

class Session {
 public:
  static constexpr uint16_t CATALOG_PAGE_SIZE = 40;
  static constexpr uint16_t COVER_BATCH = 8;
  static constexpr size_t MAX_CONFLICTS = 6;
  static constexpr uint64_t FREE_SPACE_MARGIN = 2u * 1024u * 1024u;

  // `lendFramebuffer` selects the low-heap locator path (foreground sessions,
  // which hold the render lock) versus heap-only mapping (background agent).
  Session(Client& client, FileStore& files, GfxRenderer& renderer, std::string account, bool lendFramebuffer,
          SessionCallbacks callbacks);

  // begin() + authenticate + open/reconcile the file index.
  ClientError connect();

  struct CatalogResult {
    uint16_t cached = 0;
    uint32_t serverTotal = 0;
    bool truncated = false;
  };
  // Streams the catalogue into a staging file and installs it. `beforeInstall`
  // (optional) runs after the transfer, right before the live file is
  // replaced, so a screen holding the old catalogue can let go of it.
  ClientError refreshCatalog(uint32_t fetchedAt, CatalogResult& out, void (*beforeInstall)(void*) = nullptr,
                             void* beforeInstallCtx = nullptr);

  // Missing covers, managed books first; evicts covers of books that left the
  // catalogue. With a server advertising deviceThumbnails the list tile and
  // the grid thumbnail arrive as 1-bit BMPs at the sizes set by
  // setDeviceCoverSize(); otherwise the JPEG thumbnail is fetched and decoded
  // into both renditions on the device, reusing retained JPEGs.
  void refreshCovers();
  // The grid thumbnail size to request (0 = none); set once at boot from the
  // panel's density so the Library finds the file it expects.
  static void setDeviceCoverSize(int width, int height);

  // Progress exchange for every complete managed file. A book equal to
  // `openBook.path` uses the snapshot instead of its caches and is never
  // repositioned. Conflicts go to `conflicts` when `askOnConflict`, else the
  // record is flagged for the next foreground session.
  ProgressStats syncProgress(const ReaderSnapshot& openBook, bool askOnConflict,
                             std::vector<ProgressConflict>& conflicts);
  // Resolves one conflict the user decided on.
  bool resolveConflict(const ProgressConflict& conflict, bool keepLocal);
  // The current conflict for one managed book, fetched fresh (a background
  // session only flags conflicts). `found` is false when both sides agree
  // again, in which case the flag is cleared and nothing needs deciding.
  ClientError loadConflict(uint32_t bookId, ProgressConflict& out, bool& found);

  // Manifest -> staging -> verify -> publish. On success `finalPath` names the
  // managed file and `record` its index entry.
  ClientError downloadBook(uint32_t bookId, ManagedFile& record, std::string& finalPath);
  // Pulls the server's position into the reader's saved position and echoes it
  // back under this device's id, which is what registers the device with
  // BookOrbit. Failures never block opening the book.
  void checkIn(ManagedFile& record, const std::string& finalPath);

  // Reading insights: summary, habits and the calendar month for `year`/`month`
  // (the current month when 0) from the device API's insights routes into the
  // on-card cache, keeping the newest INSIGHTS_KEEP_MONTHS months. NOT_FOUND
  // means the server predates the routes, FORBIDDEN that KOReader sync access
  // is disabled; the caller records both so
  // the screens can say so. Validates every body before it replaces a cached
  // one.
  static constexpr uint8_t INSIGHTS_KEEP_MONTHS = 3;
  ClientError refreshInsights(uint16_t year = 0, uint8_t month = 0);
  // One more calendar month (for month navigation), same validation.
  ClientError fetchInsightsMonth(uint16_t year, uint8_t month);
  // Records an insights outcome in the account's sync state: the fetch time on
  // success, the "server has no insights" flag on 404, nothing else.
  static void noteInsightsResult(SyncState& state, ClientError error, uint32_t at);

  Client& client() { return client_; }

 private:
  bool cancelled() const { return callbacks_.shouldCancel && callbacks_.shouldCancel(callbacks_.ctx); }
  void phase(SessionPhase phase, uint32_t a = 0, uint32_t b = 0);
  ClientError matchSideloads();
  ClientError syncReadingEvents(const ManagedFile& file, const std::string& path);
  bool syncOneFile(ManagedFile& file, const std::string& path, const ReaderSnapshot& openBook, bool askOnConflict,
                   std::vector<ProgressConflict>& conflicts, ProgressStats& stats, ClientError& error);
  bool verifyStagedFile(const std::string& stagingPath, const FileManifest& manifest, std::string& computedHash);
  bool publish(const std::string& stagingPath, const FileManifest& manifest, const std::string& hash,
               ManagedFile& record, std::string& finalPath);
  static bool onDownloadProgress(void* ctx, size_t received, size_t total);

  static int deviceCoverW_;
  static int deviceCoverH_;

  Client& client_;
  FileStore& files_;
  GfxRenderer& renderer_;
  std::string account_;
  bool lendFramebuffer_;
  SessionCallbacks callbacks_;
  bool cancelFlag_ = false;
};

}  // namespace bookorbit
