#include "LibraryListActivity.h"

#include <BoardConfig.h>
#include <Bitmap.h>
#include <BookOrbitCoverTile.h>
#include <BookOrbitCredentialStore.h>
#include <BookOrbitDownloadQueue.h>
#include <BookOrbitPaths.h>
#include <Epub.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <LibraryBuilder.h>
#include <LibraryRowOrder.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <optional>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/bookorbit/BookOrbitBrowseActivity.h"
#include "activities/bookorbit/BookOrbitConflictActivity.h"
#include "activities/bookorbit/BookOrbitDownloadActivity.h"
#include "activities/bookorbit/BookOrbitQueueActivity.h"
#include "activities/bookorbit/BookOrbitSyncActivity.h"
#include "activities/bookorbit/BookOrbitUi.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "bookorbit/BookOrbitAgent.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/headerIcons.h"
#include "components/icons/libraryToolbarIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/ScreenMetrics.h"

namespace fui = freeink::ui;

namespace {
constexpr int SIDE_PADDING = 12;

// Side clearance for the footer text in the panel's bottom corners.
int bottomCornerClearance() {
  const auto& board = BoardConfig::ACTIVE;
  return board.bottomCornerRadius != 0 ? board.bottomCornerRadius : board.cornerRadius;
}

constexpr unsigned long LONG_PRESS_MS = 1000;

constexpr int RECENT_TAB = 0;
constexpr int TITLE_TAB = 1;
constexpr int AUTHOR_TAB = 2;
// Only present with a BookOrbit account: the catalogue's series (a list of
// series to open, one level down the series' books), and the offline shelf.
constexpr int SERIES_TAB = 3;
constexpr int DEVICE_TAB = 4;
constexpr int LOCAL_TAB_SLOTS = AUTHOR_TAB + 1;
constexpr int REMOTE_TAB_SLOTS = DEVICE_TAB + 1;

constexpr bool isDescending(const library::SortOrder order) {
  return order == library::SortOrder::RecentDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc;
}

constexpr bool isRecentSort(const library::SortOrder order) {
  return order == library::SortOrder::RecentAsc || order == library::SortOrder::RecentDesc;
}

constexpr bool isAuthorSort(const library::SortOrder order) {
  return order == library::SortOrder::AuthorAsc || order == library::SortOrder::AuthorDesc;
}

constexpr library::SortOrder orderForTab(const int tab, const uint8_t descendingTabs) {
  const bool descending = (descendingTabs & (1u << tab)) != 0;
  if (tab == TITLE_TAB || tab == DEVICE_TAB || tab == SERIES_TAB)
    return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == AUTHOR_TAB) return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}

void formatSeriesBookTitle(const bookorbit::CatalogRecord& record, std::string& out) {
  out.assign(record.titleView());
  if (record.seriesIndexTenths == 0) return;
  char index[16];
  const unsigned tenths = record.seriesIndexTenths;
  if (tenths % 10 == 0) {
    snprintf(index, sizeof(index), "%u", tenths / 10);
  } else {
    snprintf(index, sizeof(index), "%u.%u", tenths / 10, tenths % 10);
  }
  char title[bookorbit::CATALOG_TITLE_BYTES + 32];
  snprintf(title, sizeof(title), tr(STR_LIBRARY_SERIES_BOOK_TITLE), index, out.c_str());
  out.assign(title);
}

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Library", renderer, mappedInput, true) {
  // Keep icon pills compact within their full-width touch slots.
  tabPillMaxPad = 16;
}

void LibraryListActivity::onEnter() {
  // BookOrbit adds a tab; decide before the base sizes its per-tab state.
  remoteEnabled = BOOKORBIT_STORE.isConfigured();
  if (remoteEnabled) account = BOOKORBIT_STORE.getAccountKey();

  // One lock across the base lifecycle AND the data phase: the base onEnter
  // schedules a paint, and the render task must not read the index or the
  // filter before they are in place. The rebuild also needs the lock: the
  // render task's SD-loaded fonts read glyph data at draw time, and the walk
  // needs the card to itself.
  RenderLock lock(*this);
  UiTabListActivity::onEnter();
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_REBUILD, &LibraryListActivity::rebuildActionTrampoline, this);
  app.on(ACTION_BACK, &LibraryListActivity::backActionTrampoline, this);
  app.on(ACTION_VIEW, &LibraryListActivity::viewToggleTrampoline, this);
  app.on(ACTION_REFRESH, &LibraryListActivity::refreshTrampoline, this);
  app.on(ACTION_QUEUE, &LibraryListActivity::queueTrampoline, this);

  // Recent is backed by the resident store. Prune before opening the index so
  // its persistence write never overlaps the long-lived index reader.
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();

  // Rebuild when the index is missing, invalid, or was built with the other
  // metadata mode. Otherwise entering the screen stays instant.
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  const bool rebuildNeeded = library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
                             index.header().metadataEnabled != readMetadata;
  if (rebuildNeeded) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    rebuildIndex();
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  }
  degraded = index.isOpen() && index.ranksDegraded();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }

  if (remoteEnabled) {
    if (!files.open(account)) LOG_ERR("LIB", "cannot open BookOrbit file index");
    if (!catalog.open(account)) LOG_DBG("LIB", "no BookOrbit catalogue cached yet");
    bookorbit::loadSyncState(account, syncState);
    reloadQueue();
    selectRemoteOrder();
    applyRemoteFilter();
    formatRemoteStatus(remoteStatus);
    remoteHeading = std::string(tr(STR_BOOKORBIT)) + (remoteStatus.empty() ? "" : " · " + remoteStatus);
    seenAgentGeneration = BOOKORBIT_AGENT.generation();
    BOOKORBIT_AGENT.setLibraryHoldsCatalog(catalog.isOpen());
  }
  resolvePinned();

  if (gridMode()) prepareGridWindow(/*renderLocked=*/true);

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryListActivity::loop() {
  UiTabListActivity::loop();
  // Runs after every input pass and before the deferred render, so a selection
  // that crossed onto another grid page has its thumbnails ready to draw. Also
  // answers a render that found the page larger than the prepared window.
  if (gridMode()) {
    gridNeedsPrepare = false;
    if (prepareGridWindowChanged()) {
      requestUpdate();
    } else if (generateGridCover()) {
      requestUpdate();
    }
  }
}

void LibraryListActivity::onExit() {
  closeSources();
  Activity::onExit();
}

// The index and the catalogue each hold an SD handle; sub-screens and the
// reader want the card to themselves.
void LibraryListActivity::closeSources() {
  index.close();
  catalog.close();
  BOOKORBIT_AGENT.setLibraryHoldsCatalog(false);
}

// The queue file is read on entry and whenever the agent's generation moves;
// per-row checks then cost a scan of a few ids.
void LibraryListActivity::reloadQueue() {
  queueIds.clear();
  queueFailedIds.clear();
  if (!remoteEnabled) return;
  bookorbit::DownloadQueue queue;
  if (!queue.load(account)) return;
  for (const auto& item : queue.items()) {
    queueIds.push_back(item.bookId);
    if (item.state == bookorbit::QueueState::Failed) queueFailedIds.push_back(item.bookId);
  }
}

bool LibraryListActivity::queuedBook(const uint32_t bookId) const {
  return std::find(queueIds.begin(), queueIds.end(), bookId) != queueIds.end();
}

bool LibraryListActivity::queueFailed(const uint32_t bookId) const {
  return std::find(queueFailedIds.begin(), queueFailedIds.end(), bookId) != queueFailedIds.end();
}

void LibraryListActivity::reopenSources() {
  if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
  reloadQueue();
  if (remoteEnabled && catalog.open(account)) {
    selectRemoteOrder();
    applyRemoteFilter();
    BOOKORBIT_AGENT.setLibraryHoldsCatalog(true);
  }
}

// Background agent follow-up. The catalogue handle is released while the
// agent swaps the file, and everything remote is reloaded once its generation
// moves; row states (queued / downloading) repaint on a throttle.
void LibraryListActivity::pollAgent() {
  if (!remoteEnabled || !BOOKORBIT_AGENT.available()) return;
  auto& agent = BOOKORBIT_AGENT;
  if (agent.catalogBusy()) {
    if (catalog.isOpen()) {
      RenderLock lock(*this);
      catalog.close();
      remoteRows.reset();
      remoteRowsCount = 0;
      agent.setLibraryHoldsCatalog(false);
    }
  } else if (agent.generation() != seenAgentGeneration) {
    seenAgentGeneration = agent.generation();
    {
      RenderLock lock(*this);
      catalog.close();
      if (catalog.open(account)) {
        selectRemoteOrder();
        agent.setLibraryHoldsCatalog(true);
      }
      if (!files.open(account)) LOG_ERR("LIB", "cannot reopen BookOrbit file index");
      bookorbit::loadSyncState(account, syncState);
      reloadQueue();
      applyFilter();
      formatRemoteStatus(remoteStatus);
      remoteHeading = std::string(tr(STR_BOOKORBIT)) + (remoteStatus.empty() ? "" : " · " + remoteStatus);
      auto& nav = activeNav();
      const int count = listCount();
      if (nav.selected > count) nav.selected = count;
      nav.followOnBuild = true;
    }
    closeRouting();
    requestUpdate();
    return;
  }
  const bool busy = agent.busy();
  const unsigned long now = millis();
  if (busy != agentWasBusy || (busy && now - lastAgentPaintMs >= 4000)) {
    agentWasBusy = busy;
    lastAgentPaintMs = now;
    requestUpdate();
  }
}

void LibraryListActivity::formatAgentStatus(std::string& out) const {
  out.clear();
  auto& agent = BOOKORBIT_AGENT;
  if (!agent.available()) return;
  char buf[64];
  if (agent.busy()) {
    switch (agent.phase()) {
      case bookorbit::SessionPhase::Catalog:
        out = tr(STR_BOOKORBIT_CATALOG_PROGRESS);
        break;
      case bookorbit::SessionPhase::Covers:
        snprintf(buf, sizeof(buf), tr(STR_BOOKORBIT_FETCHING_COVERS), static_cast<unsigned>(agent.phaseA()),
                 static_cast<unsigned>(agent.phaseB()));
        out = buf;
        break;
      case bookorbit::SessionPhase::Matching:
        out = tr(STR_BOOKORBIT_MATCHING_PROGRESS);
        break;
      case bookorbit::SessionPhase::ReadingEvents:
      case bookorbit::SessionPhase::Progress:
        out = tr(STR_BOOKORBIT_SYNCING_PROGRESS);
        break;
      case bookorbit::SessionPhase::Insights:
        out = tr(STR_BOOKORBIT_FETCHING_INSIGHTS);
        break;
      default:
        out = tr(STR_BOOKORBIT_SYNCING_BG);
        break;
    }
    return;
  }
  if (agent.wifiHeld() && agent.stayConnectedActive()) {
    snprintf(buf, sizeof(buf), tr(STR_WIFI_ON_MINUTES_LEFT),
             static_cast<unsigned>((agent.idleSecondsLeft() + 59) / 60));
    out = buf;
  } else if (agent.lastJobFailed()) {
    out = tr(STR_BOOKORBIT_SYNC_FAILED);
    out += ": ";
    out += bookOrbitErrorText(agent.lastError());
  }
}

bool LibraryListActivity::rebuildIndex() {
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  if (!ok) {
    LOG_ERR("LIB", "index build failed");
    return false;
  }
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed, %u enriched (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped));
  if (stats.dedupDegraded) LOG_ERR("LIB", "rebuild completed without duplicate detection");
  return true;
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

int LibraryListActivity::selectedEntry() const {
  const int entry = ringPos() - 1;
  return entry < 0 ? 0 : entry;
}

// --- BookOrbit section -------------------------------------------------------

bool LibraryListActivity::deviceTab() const { return remoteEnabled && activeTabIndex == DEVICE_TAB; }

bool LibraryListActivity::seriesTab() const { return remoteEnabled && activeTabIndex == SERIES_TAB; }

bool LibraryListActivity::authorTab() const { return activeTabIndex == AUTHOR_TAB; }

bool LibraryListActivity::authorGroupMode() const {
  return authorTab() && query.empty() && openCatalog < 0 && openLocalAuthor < 0;
}

bool LibraryListActivity::catalogGroupMode() const {
  return (seriesTab() || authorTab()) && query.empty() && openCatalog < 0 && openLocalAuthor < 0;
}

// Compact row ranges are reused between the Series and Author views. Allocate
// against the catalogue cap on the heap, with an explicit OOM path, rather than
// putting a book-sized map on the embedded task's stack.
void LibraryListActivity::buildCatalogGroups() {
  catalogGroupCount = 0;
  if (!remoteActive()) return;
  const uint16_t rows = seriesTab() ? catalog.seriesRowCount() : catalog.count();
  if (catalogGroupCapacity < rows) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(rows);
    auto sizes = makeUniqueNoThrow<uint16_t[]>(rows);
    if (!starts || !sizes) {
      LOG_ERR("LIB", "cannot allocate catalogue group map for %u rows", rows);
      filterFailed = true;
      return;
    }
    catalogGroupStart = std::move(starts);
    catalogGroupSize = std::move(sizes);
    catalogGroupCapacity = rows;
  }
  bookorbit::CatalogRecord record;
  std::string previous;
  for (uint16_t row = 0; row < rows; row++) {
    const uint16_t ordinal = catalog.ordinalForRow(row);
    if (ordinal == 0xFFFF || !catalog.readRecord(ordinal, record)) {
      catalogGroupCount = 0;
      filterFailed = true;
      return;
    }
    std::string key =
        authorTab() ? std::string(record.authorFoldView()) : library::fold(std::string(record.seriesView()));
    if (catalogGroupCount == 0 || key != previous) {
      catalogGroupStart[catalogGroupCount] = row;
      catalogGroupSize[catalogGroupCount++] = 1;
      previous = std::move(key);
    } else {
      catalogGroupSize[catalogGroupCount - 1]++;
    }
  }
  if (openCatalog >= catalogGroupCount) openCatalog = -1;
}

void LibraryListActivity::openCatalogGroup(const int group) {
  if (group < 0 || group >= static_cast<int>(catalogGroupCount)) return;
  openCatalog = group;
  applyFilter();
  auto& nav = activeNav();
  nav.selected = 1;  // the first book, with the strip keeping row 0
  nav.top = 0;
  nav.followOnBuild = true;
  if (gridMode()) prepareGridWindow();
  closeRouting();
  requestUpdate();
}

void LibraryListActivity::closeCatalogGroup() {
  const int group = openCatalog;
  openCatalog = -1;
  applyFilter();
  auto& nav = activeNav();
  const int row = remotePositionFor(group);
  nav.selected = row >= 0 ? remoteBase() + row + 1 : 0;
  nav.top = 0;
  nav.followOnBuild = true;
  closeRouting();
  requestUpdate();
}

// Queues every book of the group that is not on the card yet.
void LibraryListActivity::promptDownloadGroup(const int group) {
  if (group < 0 || group >= static_cast<int>(catalogGroupCount)) return;
  bookorbit::CatalogRecord record;
  const uint16_t start = catalogGroupStart[static_cast<size_t>(group)];
  const uint16_t size = catalogGroupSize[static_cast<size_t>(group)];
  std::string name;
  uint16_t missing = 0;
  for (uint16_t row = start; row < start + size; row++) {
    const uint16_t ordinal = catalog.ordinalForRow(row);
    if (ordinal == 0xFFFF || !catalog.readRecord(ordinal, record)) continue;
    if (name.empty()) name.assign(authorTab() ? record.authorView() : record.seriesView());
    if (!files.isComplete(record.bookId) && !queuedBook(record.bookId)) missing++;
  }
  if (missing == 0) return;
  char message[96];
  snprintf(message, sizeof(message),
           authorTab() ? tr(STR_LIBRARY_AUTHOR_DOWNLOAD_ALL) : tr(STR_LIBRARY_SERIES_DOWNLOAD_ALL),
           static_cast<unsigned>(missing));
  closeSources();
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, message, name);
  if (!confirmation) {
    reopenSources();
    return;
  }
  startActivityForResult(std::move(confirmation), [this, start, size](const ActivityResult& result) {
    swallowHeldReleases();
    {
      RenderLock lock(*this);
      reopenSources();
      if (!result.isCancelled) {
        bookorbit::DownloadQueue queue;
        const bool agent = BOOKORBIT_AGENT.available();
        if (!agent) queue.load(account);
        bookorbit::CatalogRecord record;
        for (uint16_t row = start; row < start + size; row++) {
          const uint16_t ordinal = catalog.ordinalForRow(row);
          if (ordinal == 0xFFFF || !catalog.readRecord(ordinal, record)) continue;
          if (files.isComplete(record.bookId)) continue;
          if (agent) {
            BOOKORBIT_AGENT.requestDownload(record.bookId);
          } else {
            queue.add(record.bookId);
          }
        }
        if (!agent) queue.save();
        reloadQueue();
        applyFilter();
      }
    }
    closeRouting();
    requestUpdate(true);
  });
}

// The refresh control lives in the header toolbar; the entry layout keeps
// the slot for the day a non-touch device needs a row again.
int LibraryListActivity::refreshRowCount() const { return 0; }

int LibraryListActivity::remoteRowCount() const { return remoteActive() ? remoteRowsCount : 0; }

// Recent shows newest first as its default, which the catalogue's recent
// index already is; the other orders are ascending on the card.
int LibraryListActivity::remotePositionFor(const int remoteEntry) const {
  const bool descending = (descendingTabs & (1u << activeTabIndex)) != 0;
  // An opened series keeps its index order whichever way the tab is flipped.
  const bool reversed = seriesTab() && openCatalog >= 0 ? false
                        : activeTabIndex == RECENT_TAB  ? !descending
                                                        : descending;
  return library::orderedPosition(remoteEntry, remoteRowCount(), reversed);
}

uint16_t LibraryListActivity::remoteRowFor(const int remoteEntry) const {
  const int position = remotePositionFor(remoteEntry);
  if (position < 0) return 0xFFFF;
  return remoteIdentity ? static_cast<uint16_t>(position) : remoteRows[position];
}

bool LibraryListActivity::remoteRecordFor(const int remoteEntry, bookorbit::CatalogRecord& out) {
  const uint16_t row = remoteRowFor(remoteEntry);
  if (row == 0xFFFF) return false;
  const uint16_t ordinal = catalog.ordinalForRow(row);
  return ordinal != 0xFFFF && catalog.readRecord(ordinal, out);
}

void LibraryListActivity::selectRemoteOrder() {
  if (!remoteActive()) return;
  bookorbit::CatalogOrder order = bookorbit::CatalogOrder::Title;
  if (activeTabIndex == RECENT_TAB) order = bookorbit::CatalogOrder::Recent;
  if (activeTabIndex == AUTHOR_TAB) order = bookorbit::CatalogOrder::Author;
  if (activeTabIndex == SERIES_TAB) order = bookorbit::CatalogOrder::Series;
  if (!catalog.selectOrder(order)) {
    // A cache synced before an index existed (the series index is newer than
    // the others): build the sidecars once and try again.
    catalog.close();
    const bool rebuilt = bookorbit::buildCatalogIndices(account);
    if (!catalog.open(account) || !rebuilt || !catalog.selectOrder(order)) {
      LOG_ERR("LIB", "BookOrbit order %d unavailable; title order", static_cast<int>(order));
    }
  }
}

// The surviving catalogue rows for the query and tab. Both narrowing cases
// walk every record once; the ordinary All shelf costs nothing.
void LibraryListActivity::applyRemoteFilter() {
  remoteRows.reset();
  remoteRowsCount = 0;
  remoteIdentity = true;
  if (!remoteActive() || (authorTab() && query.empty() && openLocalAuthor >= 0)) return;
  const uint16_t total = catalog.count();
  if ((seriesTab() || authorTab()) && query.empty()) {
    // One row per group, or the opened group's books.
    buildCatalogGroups();
    const size_t groups = catalogGroupCount;
    if (groups == 0) return;
    const size_t wanted = openCatalog < 0 ? groups : catalogGroupSize[static_cast<size_t>(openCatalog)];
    auto rows = makeUniqueNoThrow<uint16_t[]>(wanted);
    if (!rows) {
      LOG_ERR("LIB", "cannot allocate catalogue group rows");
      filterFailed = true;
      return;
    }
    if (openCatalog < 0) {
      for (size_t i = 0; i < groups; i++) rows[i] = catalogGroupStart[i];
    } else {
      const uint16_t start = catalogGroupStart[static_cast<size_t>(openCatalog)];
      for (size_t i = 0; i < wanted; i++) rows[i] = static_cast<uint16_t>(start + i);
    }
    remoteRows = std::move(rows);
    remoteRowsCount = static_cast<uint16_t>(wanted);
    remoteIdentity = false;
    return;
  }
  if (query.empty() && !deviceTab()) {
    remoteRowsCount = total;
    return;
  }
  if (total == 0) return;
  auto rows = makeUniqueNoThrow<uint16_t[]>(total);
  if (!rows) {
    LOG_ERR("LIB", "cannot allocate %u-byte BookOrbit filter", static_cast<unsigned>(total * sizeof(uint16_t)));
    return;
  }
  const std::string needle = query.empty() ? std::string() : library::fold(query);
  uint16_t matches = 0;
  bookorbit::CatalogRecord record;
  for (uint16_t row = 0; row < total; row++) {
    const uint16_t ordinal = catalog.ordinalForRow(row);
    if (ordinal == 0xFFFF || !catalog.readRecord(ordinal, record)) continue;
    if (deviceTab() && !files.isComplete(record.bookId) && !queuedBook(record.bookId)) continue;
    if (!needle.empty() && !library::matchesQuery(record.foldView(), needle) &&
        !library::matchesQuery(record.authorFoldView(), needle)) {
      continue;
    }
    rows[matches++] = row;
  }
  remoteRows = std::move(rows);
  remoteRowsCount = matches;
  remoteIdentity = false;
}

// Section subtitle: when the cache was last refreshed, how much of the server
// it holds, or why it is empty.
void LibraryListActivity::formatRemoteStatus(std::string& out) const {
  out.clear();
  if (!catalog.isOpen() || catalog.count() == 0) {
    if (!syncState.lastOk && syncState.lastAttemptAt != 0) {
      out = tr(STR_BOOKORBIT_SYNC_FAILED);
      out += ": ";
      out += bookOrbitErrorText(static_cast<bookorbit::ClientError>(syncState.lastErrorCode));
    } else {
      out = tr(STR_BOOKORBIT_NOT_SYNCED);
    }
    return;
  }
  const auto& head = catalog.header();
  char buf[96];
  if (head.fetchedAt != 0) {
    const time_t when = static_cast<time_t>(head.fetchedAt);
    struct tm local;
    localtime_r(&when, &local);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &local);
    snprintf(buf, sizeof(buf), tr(STR_BOOKORBIT_UPDATED), stamp);
    out = buf;
  }
  if (head.truncated) {
    snprintf(buf, sizeof(buf), tr(STR_BOOKORBIT_SHOWING_FIRST), static_cast<unsigned>(head.count),
             static_cast<unsigned long>(head.serverTotal));
    if (!out.empty()) out += " · ";
    out += buf;
  }
  if (!syncState.lastOk && syncState.lastAttemptAt != 0) {
    if (!out.empty()) out += " · ";
    out += tr(STR_BOOKORBIT_SYNC_FAILED);
  }
}

bool LibraryListActivity::loadCoverTile(const uint32_t bookId, uint8_t* tile) const {
  const std::string path = bookorbit::coverTilePath(account, bookId);
  if (!Storage.exists(path.c_str())) return false;  // not fetched yet: no error to log
  HalFile file;
  if (!Storage.openFileForRead("LIB", path.c_str(), file)) return false;
  return file.read(tile, bookorbit::COVER_TILE_BYTES) == static_cast<int>(bookorbit::COVER_TILE_BYTES);
}

void LibraryListActivity::launchRefresh() {
  app.clearTapFlash();
  if (BOOKORBIT_AGENT.available()) {
    if (!BOOKORBIT_AGENT.busy()) BOOKORBIT_AGENT.requestRefresh();
    requestUpdate();
    return;
  }
  closeSources();
  // The session reboots on exit and lands back here.
  activityManager.replaceActivity(
      std::make_unique<BookOrbitSyncActivity>(renderer, mappedInput, BookOrbitSyncActivity::ReturnTo::Library));
}

void LibraryListActivity::openRemoteEntry(const int remoteEntry) {
  if (catalogGroupMode()) {
    app.clearTapFlash();
    openCatalogGroup(remotePositionFor(remoteEntry));
    return;
  }
  bookorbit::CatalogRecord record;
  if (!remoteRecordFor(remoteEntry, record)) return;
  app.clearTapFlash();
  bookorbit::ManagedFile managed;
  if (files.isComplete(record.bookId) && files.find(record.bookId, managed)) {
    const std::string path = files.pathFor(managed);
    if (Storage.exists(path.c_str())) {
      closeSources();
      onSelectBook(path);
      return;
    }
    LOG_ERR("LIB", "managed file missing for book %lu", static_cast<unsigned long>(record.bookId));
  }
  if (BOOKORBIT_AGENT.available()) {
    // Queued for the background agent; the row shows its progress and turns
    // into an On device row when the file is published.
    if (!BOOKORBIT_AGENT.requestDownload(record.bookId)) {
      RenderLock lock(*this);
      GUI.drawPopup(renderer, tr(STR_BOOKORBIT_QUEUE_FULL));
      delay(800);
    }
    reloadQueue();
    requestUpdate();
    return;
  }
  closeSources();
  activityManager.replaceActivity(std::make_unique<BookOrbitDownloadActivity>(renderer, mappedInput, record.bookId,
                                                                              std::string(record.titleView())));
}

// Foreground conflict resolution; the screen reboots back here unless the
// agent holds the link, so the sources are released first either way.
void LibraryListActivity::launchConflictResolver(const uint32_t bookId, std::string title) {
  closeSources();
  auto activity = makeUniqueNoThrow<BookOrbitConflictActivity>(renderer, mappedInput, bookId, std::move(title));
  if (!activity) {
    LOG_ERR("LIB", "OOM: conflict activity");
    reopenSources();
    return;
  }
  activityManager.replaceActivity(std::move(activity));
}

void LibraryListActivity::promptRemoveManaged(const uint32_t bookId, const std::string& title) {
  closeSources();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_REMOVE_FROM_DEVICE), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: remove confirmation");
    reopenSources();
    return;
  }
  startActivityForResult(std::move(confirmation), [this, bookId](const ActivityResult& result) {
    swallowHeldReleases();
    {
      RenderLock lock(*this);
      if (!result.isCancelled) {
        bookorbit::ManagedFile managed;
        if (files.find(bookId, managed)) {
          const std::string path = files.pathFor(managed);
          if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
          // The reader cache stays: it holds the saved position, which a later
          // download of the same version resumes from and the next sync uploads.
          if (!files.markRemoved(bookId)) LOG_ERR("LIB", "cannot remove book %lu", static_cast<unsigned long>(bookId));
        }
      }
      reopenSources();
      if (!result.isCancelled) {
        resolvePinned();
        auto& nav = activeNav();
        const int count = listCount();
        if (count == 0) {
          nav.selected = 0;
        } else if (nav.selected > count) {
          nav.selected = count;
        }
        nav.followOnBuild = true;
      }
    }
    if (!result.isCancelled) {
      closeRouting();
      requestUpdate(true);
    }
  });
}

// --- pinned overlay ------------------------------------------------------------

// The pinned overlay applies only to the shelf that reads as "what am I up
// to": the unfiltered Recent sort, newest first. A search result is a flat
// list the reader narrowed down on purpose, and the ascending toggle asks for
// oldest-first, which pinned fresh reads would contradict.
int LibraryListActivity::pinnedCount() const {
  if (activeTabIndex != RECENT_TAB || !query.empty() || !isDescending(sortOrder)) return 0;
  return pinnedTotal;
}

const RecentBook* LibraryListActivity::pinnedBook(const int slot) const {
  if (slot < 0 || slot >= pinnedTotal) return nullptr;
  const auto& books = RECENT_BOOKS.getBooks();
  const size_t store = pinnedStore[slot];
  return store < books.size() ? &books[store] : nullptr;
}

void LibraryListActivity::resolvePinned() {
  const auto& books = RECENT_BOOKS.getBooks();
  pinnedTotal = 0;
  for (size_t i = 0; i < books.size() && pinnedTotal < RecentBooksStore::MAX_RECENT_BOOKS; i++) {
    // Managed downloads are listed in the BookOrbit section; pinning them too
    // would show every recent BookOrbit book twice.
    if (remoteActive() && bookorbit::isManagedPath(books[i].path)) continue;
    pinnedStore[pinnedTotal] = static_cast<uint8_t>(i);
    pinnedAscRows[pinnedTotal] = 0xFFFF;
    pinnedTotal++;
  }
  if (pinnedTotal > 0 && index.isOpen()) {
    library::BookIdentity identities[RecentBooksStore::MAX_RECENT_BOOKS];
    for (int i = 0; i < pinnedTotal; i++) {
      const std::string& path = books[pinnedStore[i]].path;
      identities[i].pathHash = library::clixPathHash(path.data(), path.size());
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("LIB", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!index.recentRowsFor(identities, pinnedTotal, pinnedAscRows)) {
      // Without the match the overlay would duplicate every pinned book that is
      // also in the index; better to drop the pins than to show doubles.
      LOG_ERR("LIB", "recent-book lookup failed; overlay disabled");
      pinnedTotal = 0;
    }
  }
  refreshOverlap();
}

void LibraryListActivity::refreshOverlap() {
  overlapCount = 0;
  const int total = static_cast<int>(index.bookCount());
  for (int i = 0; i < pinnedTotal; i++) {
    if (pinnedAscRows[i] == 0xFFFF || pinnedAscRows[i] >= total) continue;
    const uint16_t row =
        isDescending(sortOrder) ? static_cast<uint16_t>(total - 1 - pinnedAscRows[i]) : pinnedAscRows[i];
    overlapRows[overlapCount++] = row;
  }
  std::sort(overlapRows, overlapRows + overlapCount);
}

// --- activation ------------------------------------------------------------------

void LibraryListActivity::openSelectedBook() {
  const int entry = selectedEntry();
  if (isRemoteEntry(entry)) {
    openRemoteEntry(entry - remoteBase());
    return;
  }
  openLocalEntry(localEntryOf(entry));
}

void LibraryListActivity::openLocalEntry(const int localEntry) {
  std::string path;
  if (localEntry < pinnedCount()) {
    const RecentBook* book = pinnedBook(localEntry);
    if (!book) return;
    path = book->path;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(localEntry)));
    if (ordinal == 0xFFFF) return;

    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
      LOG_ERR("LIB", "cannot resolve path for row %d", localEntry);
      return;
    }
  }
  openBookByPath(path);
}

// Shared by row activation and the options menu: the reader screen this opens
// has its own surfaces; a lingering tap flash would gray an unrelated element
// there. The index handle is released first — on hardware only one reader can
// hold a file open at a time, and the reader is about to open files of its own.
void LibraryListActivity::openBookByPath(const std::string& path) {
  app.clearTapFlash();
  // Release the handles first: on hardware only one reader can hold a file
  // open at a time, and the reader is about to open files of its own.
  closeSources();
  onSelectBook(path);
}

void LibraryListActivity::activateIndex(const int index) {
  if (authorGroupMode() && !isRemoteEntry(index)) {
    openLocalAuthorGroup(localEntryOf(index));
  } else if (groupsCollapsed) {
    expandGroup(index);
  } else {
    openSelectedBook();
  }
}

// Row long-press prompts delete wherever grouping does not own the gesture:
// the Recent sort has no groups, and an active search is already a flat list
// the reader narrowed down on purpose ("find it, hold it, delete it").
// Unfiltered Title lists keep collapse-to-groups. Pinned rows are the
// exception: holding one offers remove-from-recents, as the old Recent tab
// did. A downloaded BookOrbit row offers remove-from-device.
bool LibraryListActivity::deleteEligible() const {
  return !groupsCollapsed && !authorGroupMode() && (!query.empty() || !groupable());
}

void LibraryListActivity::onRowLongPress(const int index) {
  if (authorGroupMode() && !isRemoteEntry(index)) {
    openLocalAuthorGroup(localEntryOf(index));
    return;
  }
  if (isRemoteEntry(index) && catalogGroupMode()) {
    promptDownloadGroup(remotePositionFor(index - remoteBase()));
    return;
  }
  if (isRemoteEntry(index)) {
    bookorbit::CatalogRecord record;
    if (remoteRecordFor(index - remoteBase(), record) && files.isComplete(record.bookId)) {
      if (files.hasConflict(record.bookId)) {
        // A flagged position conflict takes the hold: resolving it is the
        // only thing the badge asks for.
        launchConflictResolver(record.bookId, std::string(record.titleView()));
      } else {
        promptRemoveManaged(record.bookId, std::string(record.titleView()));
      }
    } else {
      activateIndex(index);
    }
    return;
  }
  const int local = localEntryOf(index);
  if (isRecentSort(sortOrder)) {
    showRecentBookOptions(local);
  } else if (local < pinnedCount()) {
    const RecentBook* book = pinnedBook(local);
    if (book) promptRemoveRecentBook(book->path, book->title);
  } else if (deleteEligible()) {
    promptDeleteBook(local);
  } else if (!groupsCollapsed && groupable()) {
    collapseGroups(local);
  } else {
    activateIndex(index);
  }
}

// Recent-shelf long-press menu (button hold and touch long-press). The first
// rows may come from RecentBooksStore; the rest are index rows sorted by
// modification time. Only store rows can be removed from recents.
void LibraryListActivity::showRecentBookOptions(const int entry) {
  if (entry < 0 || entry >= listCount()) return;

  std::string path;
  std::string title;
  const bool isStoreRow = entry < pinnedCount();
  if (isStoreRow) {
    const RecentBook* book = pinnedBook(entry);
    if (!book) return;
    path = book->path;
    title = book->title;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    std::string author;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path) ||
        !rowTextFor(entry, title, author)) {
      LOG_ERR("LIB", "cannot resolve Recent row %d", entry);
      return;
    }
  }

  const char* STORE_OPTIONS[] = {tr(STR_OPEN), tr(STR_REMOVE_FROM_RECENTS), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  const char* INDEX_OPTIONS[] = {tr(STR_OPEN), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), isStoreRow ? STORE_OPTIONS : INDEX_OPTIONS, isStoreRow ? 4 : 3, 0,
                   [this, path, title, isStoreRow](const int choice) {
                     swallowHeldReleases();
                     switch (choice) {
                       case 0:
                         openBookByPath(path);
                         break;
                       case 1:
                         if (isStoreRow) {
                           promptRemoveRecentBook(path, title);
                         } else {
                           promptDeleteBookByPath(path, title);
                         }
                         break;
                       case 2:
                         if (isStoreRow)
                           promptDeleteBookByPath(path, title);
                         else
                           promptRebuildIndex();
                         break;
                       case 3:
                         if (isStoreRow) promptRebuildIndex();
                         break;
                       default:
                         break;
                     }
                   });
  requestUpdate();
}

#if FREEINK_CAP_TOUCH
void LibraryListActivity::swipeDelete(const int entry) {
  activeNav().selected = entry + 1;
  if (isRecentSort(sortOrder) && entry < pinnedCount()) {
    const auto& book = RECENT_BOOKS.getBooks()[static_cast<size_t>(entry)];
    promptDeleteBookByPath(book.path, book.title);
  } else {
    promptDeleteBook(entry);
  }
}
#endif

// Manual index refresh, same card discipline as the onEnter rebuild: the walk
// wants the card to itself, and the render task must not read the index (or
// the filter) around it.
void LibraryListActivity::promptRebuildIndex() {
  RenderLock lock(*this);
  GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
  index.close();
  rebuildIndex();
  if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  resetAfterRebuild();
  requestUpdate(true);
}

void LibraryListActivity::resetAfterRebuild() {
  // Sort positions, group starts, and pinned rows all point into the old order.
  applyFilter();
  resolvePinned();
  auto& nav = activeNav();
  const int count = listCount();
  if (count == 0) {
    nav.selected = 0;
  } else if (nav.selected > count) {
    nav.selected = count;
  }
  nav.followOnBuild = true;
}

void LibraryListActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  closeSources();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: recent removal confirmation");
    reopenSources();
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    reopenSources();
    if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
      resolvePinned();
      closeRouting();
      auto& nav = activeNav();
      const int count = listCount();
      if (count == 0) {
        nav.selected = 0;
      } else if (nav.selected > count) {
        nav.selected = count;
      }
      nav.followOnBuild = true;
    }
  });
}

void LibraryListActivity::promptDeleteBook(const int localEntry) {
  if (!index.isOpen() || localEntry < 0 || localEntry >= bookRowCount()) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(localEntry)));
  if (ordinal == 0xFFFF) return;

  std::string path;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", localEntry);
    return;
  }
  std::string title;
  std::string author;
  rowTextFor(localEntry, title, author);
  promptDeleteBookByPath(path, title);
}

void LibraryListActivity::promptDeleteBookByPath(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  closeSources();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_ITEM_CONFIRM), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    reopenSources();
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    {
      // Same lock rationale as onEnter: the walk wants the card to itself, and
      // the render task must not read the index (or the filter) around the
      // rebuild.
      RenderLock lock(*this);
      if (!result.isCancelled) {
        LOG_DBG("LIB", "deleting %s", path.c_str());
        if (authorTab() && localAuthorSize == 1) openLocalAuthor = -1;
        if (!removeBookFile(path)) LOG_ERR("LIB", "cannot delete %s", path.c_str());
        if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
        GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
        rebuildIndex();
      }
      reopenSources();
      if (!result.isCancelled) {
        resetAfterRebuild();
      }
    }
    if (!result.isCancelled) {
      closeRouting();
      requestUpdate(true);
    }
  });
}

void LibraryListActivity::openSearch() {
  app.clearTapFlash();
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot
  // reach reads as a broken keyboard, and the eye keeps returning to it.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query, 48,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: search keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowHeldReleases();
    if (result.isCancelled) return;
    query = std::get<KeyboardResult>(result.data).text;
    openCatalog = -1;
    openLocalAuthor = -1;
    applyFilter();
    auto& nav = activeNav();
    if (!query.empty() && listCount() == 0 && !degraded) {
      // Up from the tab bar reopens Search even with no results.
      nav.selected = 0;
    } else {
      // A non-empty result belongs to the list: land on
      // its first surviving row, not on the strip.
      nav.selected = 1;
    }
    nav.top = 0;
    requestUpdate();
  });
}

void LibraryListActivity::stepTab(const int direction) {
  const int slots = tabCount();
  const int next = (activeTab() + (direction > 0 ? 1 : slots - 1)) % slots;
  selectTab(next, false);
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  selectTab(index, true);
}

void LibraryListActivity::selectTab(const int index, const bool toggleIfActive) {
#if FREEINK_CAP_TOUCH
  closeSwipeDelete();
#endif
  if (index < 0 || index >= tabCount()) return;
  if (toggleIfActive && index == activeTab()) descendingTabs ^= static_cast<uint8_t>(1u << index);
  sortOrder = orderForTab(index, descendingTabs);
  activeTabIndex = index;
  openCatalog = -1;
  openLocalAuthor = -1;
  // The filter and the overlap rows hold positions in the old order, so they
  // must be rebuilt.
  selectRemoteOrder();
  applyFilter();
  refreshOverlap();
  // Tab changes happen only while the bar owns focus. A tab's remembered row
  // must not pull focus back into the list after the switch.
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::toggleSortDirection() { selectTab(activeTab(), true); }

int LibraryListActivity::tabCount() const { return remoteEnabled ? REMOTE_TAB_SLOTS : LOCAL_TAB_SLOTS; }

int LibraryListActivity::activeTab() const { return activeTabIndex; }

const char* LibraryListActivity::tabLabel(int) const { return nullptr; }

fui::BitmapRef LibraryListActivity::tabIcon(const int index) const {
  if (index == TITLE_TAB) return fui::bitmapFromIcon(icon_library_title_24);
  if (index == AUTHOR_TAB) return fui::bitmapFromIcon(icon_library_author_24);
  if (index == SERIES_TAB) return fui::bitmapFromIcon(icon_library_series_24);
  if (index == DEVICE_TAB) return fui::bitmapFromIcon(icon_library_device_24);
  return fui::bitmapFromIcon(icon_library_recent_24);
}

fui::TabIndicator LibraryListActivity::tabIndicator(const int index) const {
  if (index != activeTab()) return fui::TabIndicator::None;
  return isDescending(sortOrder) ? fui::TabIndicator::Down : fui::TabIndicator::Up;
}

int LibraryListActivity::bookRowCount() const {
  if (authorTab() && query.empty()) {
    if (openCatalog >= 0) return 0;
    if (localAuthorSize >= 0) return localAuthorSize;
  }
  if (seriesTab()) return 0;  // sideloaded files carry no series data
  if (!query.empty()) return static_cast<int>(filteredCount);
  // Pinned books already in the index are skipped below the pins, not doubled;
  // pinned books the index missed still show, so the difference stays split.
  const int pinned = pinnedCount();
  return static_cast<int>(index.bookCount()) + (pinned > 0 ? pinned - overlapCount : 0);
}

int LibraryListActivity::listCount() const {
  return groupsCollapsed ? static_cast<int>(groupCount)
                         : localBase() + (authorGroupMode() ? groupCount : bookRowCount());
}

// Local entry position to row position in the sort order. Identity while
// unfiltered and unpinned, so the shelf costs nothing when nothing is typed.
// With pins active, entries below pinnedCount() belong to the store and must
// not reach this; the rest walk past the pinned books' own sort rows.
int LibraryListActivity::rowFor(const int localEntry) const {
  if (!query.empty()) {
    if (localEntry < 0 || localEntry >= static_cast<int>(filteredCount) || !filtered) return 0;
    return filtered[localEntry];
  }
  if (authorTab() && query.empty() && localAuthorSize >= 0) return localAuthorStart + localEntry;
  const int pinned = pinnedCount();
  if (pinned == 0) return localEntry;
  int row = localEntry - pinned;
  for (int i = 0; i < overlapCount; i++) {
    if (overlapRows[i] <= row) row++;
  }
  return row;
}

// Collapse-to-groups is a single-source affordance: with a BookOrbit section
// on the shelf the two sections keep their own headings instead.
bool LibraryListActivity::groupable() const {
  return !remoteEnabled && !degraded && !authorTab() && !isRecentSort(sortOrder) && bookRowCount() > 0;
}

uint32_t LibraryListActivity::titleInitialFor(const int localEntry) {
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(localEntry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return 0;
  return library::foldedGroupInitial(std::string_view(record.fold, record.foldLen));
}

bool LibraryListActivity::buildGroupStarts() {
  const int count = bookRowCount();
  if (count <= 0) return false;
  if (groupCapacity < count) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(count));
    if (!starts) {
      LOG_ERR("LIB", "cannot allocate %u-byte group map", static_cast<unsigned>(count * sizeof(uint16_t)));
      return false;
    }
    groupStarts = std::move(starts);
    groupCapacity = static_cast<uint16_t>(count);
  }

  groupCount = 0;
  uint32_t previousInitial = 0;
  std::string previousAuthor;
  std::string title;
  std::string author;
  previousAuthor.reserve(128);
  title.reserve(128);
  author.reserve(128);
  for (int entry = 0; entry < count; entry++) {
    bool startsGroup = entry == 0;
    if (isAuthorSort(sortOrder)) {
      rowTextFor(entry, title, author);
      startsGroup = startsGroup || author != previousAuthor;
      previousAuthor = author;
    } else {
      const uint32_t initial = titleInitialFor(entry);
      startsGroup = startsGroup || initial != previousInitial;
      previousInitial = initial;
    }
    if (startsGroup) groupStarts[groupCount++] = static_cast<uint16_t>(entry);
  }
  LOG_DBG("LIB", "group map: %u groups, %u bytes", static_cast<unsigned>(groupCount),
          static_cast<unsigned>(groupCapacity * sizeof(uint16_t)));
  return groupCount > 0;
}

int LibraryListActivity::groupForBook(const int bookEntry) const {
  int group = 0;
  while (group + 1 < groupCount && groupStarts[group + 1] <= bookEntry) group++;
  return group;
}

bool LibraryListActivity::collapseGroups(const int bookEntry) {
  if (!groupable() || !buildGroupStarts()) return false;
  expandedNav = activeNav();
  groupsCollapsed = true;
  auto& nav = activeNav();
  nav.reset(groupForBook(bookEntry) + 1);
  requestUpdate();
  return true;
}

void LibraryListActivity::expandGroup(const int groupEntry) {
  if (!groupsCollapsed || groupEntry < 0 || groupEntry >= groupCount) return;
  const int bookEntry = groupStarts[groupEntry];
  groupsCollapsed = false;
  activeNav() = expandedNav;
  auto& nav = activeNav();
  nav.selected = bookEntry + 1;
  nav.top = bookEntry;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::openLocalAuthorGroup(const int group) {
  if (group < 0 || group >= groupCount) return;
  openLocalAuthor = group;
  applyFilter();
  activeNav().reset(1);
  if (gridMode()) prepareGridWindow();
  closeRouting();
  requestUpdate();
}

void LibraryListActivity::closeLocalAuthorGroup() {
  const int group = openLocalAuthor;
  openLocalAuthor = -1;
  applyFilter();
  activeNav().reset(localBase() + group + 1);
  closeRouting();
  requestUpdate();
}

void LibraryListActivity::restoreExpandedList() {
  if (!groupsCollapsed) return;
  groupsCollapsed = false;
  activeNav() = expandedNav;
  requestUpdate();
}

// One pass over the sort order, keeping what matches. No index, no cache: at the
// 4096-book format cap this is 4096 comparisons of at most 96 bytes. The result
// array is allocated once with the exact upper bound and fails back to an
// explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  // A different group can have the same page size and offset as the last one.
  gridWindowTop = -1;
  groupsCollapsed = false;
  groupCount = 0;
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  // The header shows the active query in place of the screen title, so the
  // reader can see what narrowed the list without reopening the keyboard.
  headerSearchTitle = query.empty() ? std::string() : "“" + query + "”";
  localAuthorSize = -1;
  applyRemoteFilter();
  if (query.empty()) {
    if (authorTab() && openCatalog < 0) {
      if (!buildGroupStarts() && index.bookCount() > 0) filterFailed = true;
      if (openLocalAuthor >= groupCount) {
        openLocalAuthor = -1;
        applyRemoteFilter();
      }
      if (openLocalAuthor >= 0) {
        const auto range = library::groupRange(groupStarts.get(), groupCount, bookRowCount(), openLocalAuthor);
        localAuthorStart = range.start;
        localAuthorSize = range.size;
      }
    }
    return;
  }

  const std::string needle = library::fold(query);
  const int total = static_cast<int>(index.bookCount());
  if (total <= 0) return;

  auto matches = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(total));
  if (!matches) {
    LOG_ERR("LIB", "cannot allocate %u-byte search result buffer", static_cast<unsigned>(total * sizeof(uint16_t)));
    filterFailed = true;
    return;
  }

  uint16_t matchCount = 0;
  std::string author;
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    // The stored fold covers the title only, so the author has to be read and
    // folded here. That is the search most worth having: the reader who knows
    // the author usually also knows where the book is, while "emily" finding
    // Alice Hunter is the case the shelf exists to answer.
    author.clear();
    if (index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
    }
  }
  filtered = std::move(matches);
  filteredCount = matchCount;
}

// Staged back-out, shared by the Back button and the header's back arrow:
// clear the search, expand collapsed groups, return focus to the tabs, then
// leave for home.
void LibraryListActivity::handleBackAction() {
  auto& nav = activeNav();
  if (!query.empty()) {
    query.clear();
    applyFilter();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
  } else if ((seriesTab() || authorTab()) && openCatalog >= 0) {
    closeCatalogGroup();
  } else if (authorTab() && openLocalAuthor >= 0) {
    closeLocalAuthorGroup();
  } else if (groupsCollapsed) {
    restoreExpandedList();
  } else if (!tabsFocused() && !degraded) {
    // Keep the current list and viewport while returning focus to the tabs.
    nav.selected = 0;
    requestUpdate();
  } else {
    onGoHome();
  }
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::backActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->handleBackAction();
}

void LibraryListActivity::rebuildActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->promptRebuildIndex();
}

// Title and author for one local entry, read straight from the index. Only
// ever called for rows about to be drawn, so at most a screenful of strings
// exists at once.
bool LibraryListActivity::rowTextFor(const int localEntry, std::string& title, std::string& author,
                                     std::string* fileName) {
  title.clear();
  author.clear();
  if (fileName) fileName->clear();
  if (localEntry < pinnedCount()) {
    const RecentBook* book = pinnedBook(localEntry);
    if (!book) return false;
    title = book->title;
    author = book->author;
    if (fileName) *fileName = book->path;
    return true;
  }
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(localEntry)));
  library::ClixRecord record{};
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise.
    if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
    if (fileName) index.readName(record, *fileName);
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

bool LibraryListActivity::handleCustomInput() {
  pollAgent();
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (gridMode() && !tabsFocused()) {
    // The base scrolls the list viewport on swipes; the grid pages by
    // selection instead, so the prepared window follows.
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int count = listCount();
      const int page = currentGridGeometry().pageItems();
      int entry = selectedEntry() + (swipe == MappedInputManager::SwipeDir::Up ? page : -page);
      if (entry < 0) entry = 0;
      if (entry >= count) entry = count - 1;
      if (count > 0) moveRingTo(entry + 1);
      return true;
    }
  }
  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }

  return false;
}

bool LibraryListActivity::handleButtons() {
  if (remoteEnabled && tabsFocused() && mappedInput.wasLongPressed(MappedInputManager::Button::Up, LONG_PRESS_MS)) {
    openOnlineLibrary();
    return true;
  }
  const int count = listCount();

  // Every hold action fires at the threshold, mid-hold, including the ones
  // that open a dialog (remove-recent, delete). The release that follows is
  // armed as suppressed by wasLongPressed() and consumed globally by
  // ActivityManager::loop() before any activity runs, so it cannot land in
  // the freshly opened confirmation and select its default.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (tabsFocused()) {
      if (!degraded) toggleSortDirection();
    } else if (count > 0) {
      onRowLongPress(selectedEntry());
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBackAction();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      stepTab(1);
      return true;
    }
    if (count > 0) activateIndex(selectedEntry());
    return true;
  }

  return false;
}

void LibraryListActivity::navigateButtons() {
  const int count = listCount();
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = tabsFocused();
  }
  buttonNavigator.onNextPress([this, count] {
    if (count > 0) moveRingTo(ringPos() == count ? 1 : ringPos() + 1);
  });
  buttonNavigator.onPreviousPress([this, count] {
    if ((!navigationStartedOnTabs || degraded) && count > 0) {
      moveRingTo(ringPos() <= 1 ? count : ringPos() - 1);
    }
  });
  // Search is an activation: defer it so holding Previous can still step tabs.
  buttonNavigator.onPreviousRelease([this] {
    if (navigationStartedOnTabs && tabsFocused() && !degraded) openSearch();
  });
  // A held button steps tabs while the strip has focus (the base behaviour
  // Settings keeps) and page-jumps once the selection is down in the rows,
  // where fast travel through a long shelf is what a hold means.
  buttonNavigator.onNextContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, activeNav().pageRows()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, activeNav().pageRows()) + 1);
    }
  });
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  const bool authorGrouped = isAuthorSort(sortOrder);
  // Letter/author headings belong to the single-source shelf; with BookOrbit
  // the two section headings carry the structure instead.
  const bool grouped = !isRecentSort(sortOrder) && !remoteEnabled;

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  // Breathing room between rows; the dense theme default packs the two-line
  // rows edge-to-edge.
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  syncTabListViewport(screen, props);

  // Keep one extra entry in the reusable window for a clipped trailing row.
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (!groupsCollapsed && winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);
  if (remoteActive() && winTilesCap < cap) {
    // Fallible: without tiles the rows fall back to the generic book icon.
    winTiles = makeUniqueNoThrow<uint8_t[]>(cap * bookorbit::COVER_TILE_BYTES);
    winTilesCap = winTiles ? cap : 0;
  }

  int rows = 0;
  int headers = 0;
  uint32_t previousInitial = 0;
  std::string rowFile;
  rowFile.reserve(128);
  bookorbit::CatalogRecord record;
  // At most one row can be downloading, so one badge buffer serves the page.
  char rowBadge[32] = {};
  // Capture this after syncTabListViewport(), which may clamp nav.top.
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && rows < static_cast<int>(cap); entry++) {
    std::string& title = winTitles[static_cast<size_t>(rows)];
    std::string& author = winAuthors[static_cast<size_t>(rows)];
    fui::ListItem item;
    if (groupsCollapsed) {
      const int bookEntry = groupStarts[entry];
      if (authorGrouped) {
        rowTextFor(bookEntry, title, author);
        formatAuthorHeading(author, title);
      } else {
        formatInitialHeading(titleInitialFor(bookEntry), title);
      }
    } else if (isRemoteEntry(entry) && catalogGroupMode()) {
      // A series or author group, with a book count and representative cover.
      if (!remoteRecordFor(entry - remoteBase(), record)) continue;
      if (authorTab()) {
        author.assign(record.authorView());
        formatAuthorHeading(author, title);
        author.clear();
      } else {
        title.assign(record.seriesView());
        author.assign(record.authorView());
      }
      if (!author.empty()) item.subtitle = author.c_str();
      const int group = remotePositionFor(entry - remoteBase());
      const uint16_t size =
          group >= 0 && group < static_cast<int>(catalogGroupCount) ? catalogGroupSize[static_cast<size_t>(group)] : 0;
      snprintf(rowBadge, sizeof(rowBadge), tr(STR_LIBRARY_SERIES_BOOKS), static_cast<unsigned>(size));
      // Each count must remain alive until screen.list() consumes the window.
      winHeaders[static_cast<size_t>(rows)] = rowBadge;
      item.value = winHeaders[static_cast<size_t>(rows)].c_str();
      bool haveTile = false;
      if (winTiles && record.hasCover()) {
        uint8_t* tile = winTiles.get() + static_cast<size_t>(rows) * bookorbit::COVER_TILE_BYTES;
        if (loadCoverTile(record.bookId, tile)) {
          item.icon.data = tile;
          item.icon.width = bookorbit::COVER_TILE_SIZE;
          item.icon.height = bookorbit::COVER_TILE_SIZE;
          item.icon.format = fui::BitmapFormat::BW1;
          item.icon.progmem = false;
          haveTile = true;
        }
      }
      if (!haveTile) item.icon = listIconFor(UIIcon::Library, 32);
      if (entry == remoteBase()) item.sectionHeading = remoteHeading.c_str();
    } else if (isRemoteEntry(entry)) {
      if (!remoteRecordFor(entry - remoteBase(), record)) continue;
      if (seriesTab() && openCatalog >= 0) {
        formatSeriesBookTitle(record, title);
      } else {
        title.assign(record.titleView());
      }
      author.assign(record.authorView());
      if (!author.empty()) item.subtitle = author.c_str();
      const bool onDevice = files.isComplete(record.bookId);
      if (BOOKORBIT_AGENT.downloadingBookId() == record.bookId) {
        const uint32_t total = BOOKORBIT_AGENT.phaseB();
        const int pct = total > 0 ? static_cast<int>(static_cast<uint64_t>(BOOKORBIT_AGENT.phaseA()) * 100 / total) : 0;
        snprintf(rowBadge, sizeof(rowBadge), tr(STR_BOOKORBIT_DOWNLOADING_PCT), pct);
        item.value = rowBadge;
      } else if (queuedBook(record.bookId)) {
        item.value = queueFailed(record.bookId) ? tr(STR_BOOKORBIT_QUEUE_FAILED) : tr(STR_BOOKORBIT_QUEUED);
      } else if (onDevice && files.hasConflict(record.bookId)) {
        item.value = tr(STR_BOOKORBIT_CONFLICT_BADGE);
      } else {
        item.value = onDevice ? tr(STR_BOOKORBIT_ON_DEVICE) : tr(STR_BOOKORBIT_IN_LIBRARY);
      }
      bool haveTile = false;
      if (winTiles && record.hasCover()) {
        uint8_t* tile = winTiles.get() + static_cast<size_t>(rows) * bookorbit::COVER_TILE_BYTES;
        if (loadCoverTile(record.bookId, tile)) {
          item.icon.data = tile;
          item.icon.width = bookorbit::COVER_TILE_SIZE;
          item.icon.height = bookorbit::COVER_TILE_SIZE;
          item.icon.format = fui::BitmapFormat::BW1;
          item.icon.progmem = false;
          haveTile = true;
        }
      }
      if (!haveTile) item.icon = listIconFor(UIIcon::Book, 32);
      if (entry == remoteBase()) item.sectionHeading = deviceTab() ? tr(STR_BOOKORBIT) : remoteHeading.c_str();
    } else if (authorGroupMode()) {
      const int group = localEntryOf(entry);
      if (!rowTextFor(groupStarts[group], title, author)) continue;
      formatAuthorHeading(author, title);
      const auto range = library::groupRange(groupStarts.get(), groupCount, bookRowCount(), group);
      snprintf(rowBadge, sizeof(rowBadge), tr(STR_LIBRARY_SERIES_BOOKS), static_cast<unsigned>(range.size));
      winHeaders[static_cast<size_t>(rows)] = rowBadge;
      item.value = winHeaders[static_cast<size_t>(rows)].c_str();
      item.icon = fui::bitmapFromIcon(icon_library_author_24);
      if (remoteEnabled && entry == localBase()) item.sectionHeading = tr(STR_LIBRARY_LOCAL_ONLY_SECTION);
    } else {
      const int local = localEntryOf(entry);
      if (!rowTextFor(local, title, author, &rowFile)) continue;
      uint32_t initial = 0;
      bool startsGroup = false;
      if (remoteEnabled) {
        startsGroup = entry == localBase();
      } else if (authorGrouped) {
        startsGroup = rows == 0 || author != winAuthors[static_cast<size_t>(rows - 1)];
      } else if (grouped) {
        initial = titleInitialFor(local);
        startsGroup = rows == 0 || initial != previousInitial;
        previousInitial = initial;
      }
      if (startsGroup) {
        std::string& heading = winHeaders[static_cast<size_t>(headers++)];
        if (remoteEnabled)
          heading = tr(STR_LIBRARY_LOCAL_ONLY_SECTION);
        else if (authorGrouped)
          formatAuthorHeading(author, heading);
        else
          formatInitialHeading(initial, heading);
        item.sectionHeading = heading.c_str();
      }
      if ((remoteEnabled || !authorGrouped) && !author.empty()) item.subtitle = author.c_str();
      // Group headings stay bare; every book row gets its file-type icon.
      if (!rowFile.empty()) item.icon = listIconFor(UITheme::getFileIcon(rowFile), 32);
    }

    item.label = title.c_str();
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    rows++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
#if FREEINK_CAP_TOUCH
  configureSwipeDelete(screen, props);
#endif
  screen.list(props);
  const int next = nav.drawnRows;
  const auto body = screen.body();
  LOG_DBG("LIB", "page tab=%d top=%d full=%d loaded=%d body=%d..%d next=%d title=%s", activeTabIndex, windowStart,
          nav.drawnRows, rows, body.y, body.bottom(),
          next < rows ? winItems[static_cast<size_t>(next)].actionValue : -1,
          next < rows ? winItems[static_cast<size_t>(next)].label : "<none>");
  LOG_DBG("LIB", "page first=%d title=%s", rows > 0 ? winItems[0].actionValue : -1,
          rows > 0 ? winItems[0].label : "<none>");
}

// --- Grid view -----------------------------------------------------------------

namespace {
constexpr int16_t GRID_GAP = 8;
constexpr int16_t GRID_LABEL_GAP = 3;
constexpr int16_t GRID_CELL_PAD = 4;
constexpr int16_t GRID_BADGE = 24;

void formatSeriesLine(const std::string_view series, const std::string_view index, std::string& out) {
  out.clear();
  if (series.empty()) return;
  if (index.empty()) {
    out.assign(series);
    return;
  }
  char buf[96];
  snprintf(buf, sizeof(buf), tr(STR_LIBRARY_SERIES_FORMAT), std::string(series).c_str(), std::string(index).c_str());
  out = buf;
}

void formatTenths(const uint16_t tenths, std::string& out) {
  out.clear();
  if (tenths == 0) return;
  char buf[16];
  if (tenths % 10 == 0) {
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(tenths / 10));
  } else {
    snprintf(buf, sizeof(buf), "%u.%u", static_cast<unsigned>(tenths / 10), static_cast<unsigned>(tenths % 10));
  }
  out = buf;
}
}  // namespace

bool LibraryListActivity::gridMode() const {
  return SETTINGS.libraryViewMode == CrossPointSettings::LIBRARY_VIEW_GRID && !groupsCollapsed && !catalogGroupMode() &&
         !authorGroupMode();
}

LibraryListActivity::GridGeometry LibraryListActivity::gridGeometry(const int bodyWidth, const int bodyHeight) const {
  GridGeometry g;
  g.lineH = static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID));
  g.coverW = static_cast<int16_t>(screen_metrics::gridCoverWidth());
  g.coverH = static_cast<int16_t>(screen_metrics::gridCoverHeight());
  g.gap = GRID_GAP;
  const int cellMin = g.coverW + GRID_CELL_PAD * 2;
  int columns = (bodyWidth + g.gap) / (cellMin + g.gap);
  if (columns < 1) columns = 1;
  if (columns > 12) columns = 12;
  g.columns = static_cast<uint8_t>(columns);
  // Cover, then title / author / series lines.
  g.cellH = static_cast<int16_t>(g.coverH + GRID_LABEL_GAP + g.lineH * 3 + GRID_CELL_PAD);
  int rows = (bodyHeight + g.gap) / (g.cellH + g.gap);
  if (rows < 1) rows = 1;
  g.rows = static_cast<uint16_t>(rows);
  return g;
}

// The body the last render measured, or an estimate from the theme metrics
// before the first one (the render reports back if the page turns out larger).
LibraryListActivity::GridGeometry LibraryListActivity::currentGridGeometry() const {
  if (gridBodyW > 0 && gridBodyH > 0) return gridGeometry(gridBodyW, gridBodyH);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int tabBand = mappedInput.hasTouch() ? 50 : metrics.tabBarHeight;
  const int readout = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int height = renderer.getScreenHeight() - metrics.topPadding - metrics.headerHeight - tabBand -
                     metrics.buttonHintsHeight - readout;
  return gridGeometry(renderer.getScreenWidth(), height > 0 ? height : 1);
}

int LibraryListActivity::gridPageTop(const int entry, const GridGeometry& g) const {
  const int page = g.pageItems();
  return entry < 0 || page <= 0 ? 0 : (entry / page) * page;
}

void LibraryListActivity::toggleViewMode() {
  app.clearTapFlash();
  SETTINGS.libraryViewMode = gridMode() ? CrossPointSettings::LIBRARY_VIEW_LIST : CrossPointSettings::LIBRARY_VIEW_GRID;
  SETTINGS.saveToFile();
  closeRouting();
  if (gridMode()) prepareGridWindow();
  requestUpdate();
}

void LibraryListActivity::viewToggleTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->toggleViewMode();
}

// Local EPUB: the reader's own thumbnail cache (thumb_<h>.bmp next to book.bin).
// Generating one needs the book's metadata cache, which is built here when the
// book was never opened; that is the one-time cost of a cover-first shelf.
bool LibraryListActivity::ensureLocalCover(const std::string& epubPath, GridCell& cell, const bool generate) {
  if (!FsHelpers::hasEpubExtension(epubPath)) return false;
  Epub epub(epubPath, "/.crosspoint");
  const int height = currentGridGeometry().coverH;
  std::string thumb = epub.getThumbBmpPath(height);
  bool haveThumb = Storage.exists(thumb.c_str());
  bool haveCache = haveThumb;
  if (!haveThumb && generate) {
    epub.setupCacheDir();
    if (epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true)) {
      haveCache = true;
      haveThumb = epub.generateThumbBmp(height);
      formatSeriesLine(epub.getSeries(), epub.getSeriesIndex(), cell.series);
      cell.thumbPath = haveThumb ? thumb : std::string();
      return haveThumb;
    }
  }
  cell.thumbPath = haveThumb ? thumb : std::string();
  if (haveCache) {
    // Cached metadata only; a never-opened book is not parsed just for its
    // series line.
    std::string title;
    std::string author;
    std::string series;
    std::string index;
    if (epub.loadMetadata(title, author, series, index)) formatSeriesLine(series, index, cell.series);
  }
  return haveThumb;
}

// BookOrbit book: a thumbnail rendered from the JPEG the sync session keeps
// beside its list tile.
bool LibraryListActivity::ensureRemoteCover(const uint32_t bookId, GridCell& cell, const bool generate) {
  const auto g = currentGridGeometry();
  const std::string thumb = bookorbit::coverThumbPath(account, bookId, g.coverH);
  if (Storage.exists(thumb.c_str())) {
    cell.thumbPath = thumb;
    return true;
  }
  if (!generate) return false;
  const std::string jpeg = bookorbit::coverJpegPath(account, bookId);
  if (!Storage.exists(jpeg.c_str())) return false;
  HalFile in;
  if (!Storage.openFileForRead("LIB", jpeg.c_str(), in)) return false;
  const std::string tmp = thumb + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite("LIB", tmp.c_str(), out)) return false;
  bool ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(in, out, g.coverW, g.coverH);
  if (!out.close()) ok = false;
  in.close();
  if (!ok || !Storage.rename(tmp.c_str(), thumb.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  cell.thumbPath = thumb;
  return true;
}

// Fills gridCells for the page holding the selection: titles, authors, series
// and the thumbnail paths that already exist. Missing thumbnails are left to
// generateGridCover() so the page paints first. Cheap when already prepared.
bool LibraryListActivity::prepareGridWindowChanged(const bool renderLocked) {
  // The render task reads gridCells; swapping them needs the render lock,
  // which is NOT recursive: a caller already holding it (onEnter's data
  // phase) says so, or the main task deadlocks on itself.
  std::optional<RenderLock> lock;
  const int count = listCount();
  if (!gridMode() || count == 0) {
    if (!renderLocked) lock.emplace(*this);
    const bool had = !gridCells.empty();
    gridCells.clear();
    gridWindowTop = -1;
    gridWindowCount = 0;
    return had;
  }
  const GridGeometry g = currentGridGeometry();
  const int top = gridPageTop(selectedEntry(), g);
  const int pageCount = std::min(g.pageItems(), count - top);
  if (top == gridWindowTop && pageCount == gridWindowCount) return false;

  std::vector<GridCell> cells;
  cells.reserve(static_cast<size_t>(pageCount));
  bookorbit::CatalogRecord record;
  std::string fileName;
  for (int i = 0; i < pageCount; i++) {
    GridCell cell;
    cell.entry = top + i;
    if (isRemoteEntry(cell.entry)) {
      if (remoteRecordFor(cell.entry - remoteBase(), record)) {
        if (seriesTab() && openCatalog >= 0) {
          formatSeriesBookTitle(record, cell.title);
        } else {
          cell.title.assign(record.titleView());
        }
        cell.author.assign(record.authorView());
        std::string index;
        formatTenths(record.seriesIndexTenths, index);
        formatSeriesLine(record.seriesView(), index, cell.series);
        cell.remoteOnly = !files.isComplete(record.bookId);
        if (record.hasCover()) ensureRemoteCover(record.bookId, cell, false);
      }
    } else {
      rowTextFor(localEntryOf(cell.entry), cell.title, cell.author, &fileName);
      // fileName is the on-card path for pinned rows and the bare name for
      // index rows; the thumbnail lookup needs the full path either way.
      std::string path;
      const int local = localEntryOf(cell.entry);
      if (local < pinnedCount()) {
        if (const RecentBook* book = pinnedBook(local)) path = book->path;
      } else if (index.isOpen()) {
        const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(local)));
        library::ClixRecord clix{};
        if (ordinal != 0xFFFF && index.readRecord(ordinal, clix)) index.readPath(clix, path);
      }
      cell.bookPath = path;
      if (!path.empty()) ensureLocalCover(path, cell, false);
    }
    cells.push_back(std::move(cell));
  }

  // Missing thumbnails are generated one per loop pass (generateGridCover)
  // after the page is on screen, so a page turn never waits for decoding.
  if (!renderLocked) lock.emplace(*this);
  gridCells.swap(cells);
  gridGenerated = 0;
  gridGenDirty = 0;
  gridWindowTop = top;
  gridWindowCount = pageCount;
  return true;
}

void LibraryListActivity::prepareGridWindow(const bool renderLocked) { prepareGridWindowChanged(renderLocked); }

// One missing thumbnail of the prepared page, generated under the render lock
// (the JPEG decoder and the card, like the Home screen's covers). Returns true
// when a repaint is due: every few covers, and once the page is complete.
bool LibraryListActivity::generateGridCover() {
  if (gridWindowTop < 0 || gridGenerated >= GRID_MAX_GENERATE || BOOKORBIT_AGENT.busy()) return false;
  bookorbit::CatalogRecord record;
  for (auto& cell : gridCells) {
    if (!cell.thumbPath.empty() || cell.generationTried) continue;
    bool generated = false;
    if (isRemoteEntry(cell.entry)) {
      if (!remoteRecordFor(cell.entry - remoteBase(), record) || !record.hasCover()) {
        cell.generationTried = true;
        continue;
      }
      RenderLock lock(*this);
      cell.generationTried = true;
      generated = ensureRemoteCover(record.bookId, cell, true);
    } else if (!cell.bookPath.empty() && FsHelpers::hasEpubExtension(cell.bookPath)) {
      RenderLock lock(*this);
      cell.generationTried = true;
      generated = ensureLocalCover(cell.bookPath, cell, true);
    } else {
      cell.generationTried = true;
      continue;
    }
    gridGenerated++;
    if (generated) gridGenDirty++;
    const bool more = std::any_of(gridCells.begin(), gridCells.end(),
                                  [](const GridCell& c) { return c.thumbPath.empty() && !c.generationTried; });
    if (gridGenDirty > 0 && (!more || gridGenDirty >= GRID_REPAINT_EVERY || gridGenerated >= GRID_MAX_GENERATE)) {
      gridGenDirty = 0;
      return true;
    }
    return false;
  }
  return false;
}

fui::CoverGridItem LibraryListActivity::gridItemProvider(const uint16_t index, void* user) {
  auto* self = static_cast<LibraryListActivity*>(user);
  fui::CoverGridItem item;
  item.actionValue = static_cast<int16_t>(index);
  const int offset = static_cast<int>(index) - self->gridWindowTop;
  if (self->gridWindowTop >= 0 && offset >= 0 && offset < static_cast<int>(self->gridCells.size())) {
    item.title = self->gridCells[static_cast<size_t>(offset)].title.c_str();
  } else {
    // The render found more cells than the loop prepared (first paint uses
    // an estimated page); ask for another pass.
    self->gridNeedsPrepare = true;
  }
  return item;
}

bool LibraryListActivity::gridCoverPainter(fui::DrawTarget& target, const fui::Rect rect, const fui::CoverGridItem&,
                                           const uint16_t index, void* user) {
  auto* self = static_cast<LibraryListActivity*>(user);
  const int offset = static_cast<int>(index) - self->gridWindowTop;
  const GridCell* cell = (self->gridWindowTop >= 0 && offset >= 0 && offset < static_cast<int>(self->gridCells.size()))
                             ? &self->gridCells[static_cast<size_t>(offset)]
                             : nullptr;
  const fui::Paint black = fui::Paint::solid(fui::Color::Black);
  bool drawn = false;
  if (cell && !cell->thumbPath.empty()) {
    HalFile file;
    if (Storage.openFileForRead("LIB", cell->thumbPath.c_str(), file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        // drawBitmap1Bit scales down to fit; center the fitted image.
        float scale = 1.0f;
        if (bitmap.getWidth() > rect.width) scale = static_cast<float>(rect.width) / bitmap.getWidth();
        if (bitmap.getHeight() * scale > rect.height) scale = static_cast<float>(rect.height) / bitmap.getHeight();
        const int w = static_cast<int>(bitmap.getWidth() * scale);
        const int h = static_cast<int>(bitmap.getHeight() * scale);
        const int x = rect.x + (rect.width - w) / 2;
        const int y = rect.y + (rect.height - h) / 2;
        drawn = self->renderer.drawBitmap1Bit(bitmap, x, y, rect.width, rect.height);
        if (drawn)
          target.stroke(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w),
                                  static_cast<int16_t>(h)},
                        black, 1);
      }
    }
  }
  if (!drawn) {
    // No cover: a light frame with the book glyph, like the list's fallback icon.
    target.fill(rect, fui::Paint::solid(fui::Color::White));
    target.stroke(rect, black, 1);
    const fui::BitmapRef icon = listIconFor(UIIcon::Book, 32);
    if (icon) {
      const fui::Rect iconRect{static_cast<int16_t>(rect.x + (rect.width - icon.width) / 2),
                               static_cast<int16_t>(rect.y + (rect.height - icon.height) / 2),
                               static_cast<int16_t>(icon.width), static_cast<int16_t>(icon.height)};
      target.bitmap(iconRect, icon, fui::BitmapMode::Center, black);
    }
  }
  if (cell && cell->remoteOnly) {
    // Not on the card yet: a download badge in the corner.
    const fui::Rect badge{static_cast<int16_t>(rect.right() - GRID_BADGE - 2),
                          static_cast<int16_t>(rect.bottom() - GRID_BADGE - 2), GRID_BADGE, GRID_BADGE};
    target.fill(badge, fui::Paint::solid(fui::Color::White));
    target.stroke(badge, black, 1);
    target.bitmap(badge, fui::bitmapFromIcon(icon_download_24), fui::BitmapMode::Center, black);
  }
  return true;
}

void LibraryListActivity::buildGrid(UiScreen& screen) {
  const fui::Rect body = screen.body();
  gridBodyW = body.width;
  gridBodyH = body.height;
  const GridGeometry g = gridGeometry(body.width, body.height);
  const int count = listCount();
  const int top = gridPageTop(selectedEntry(), g);

  fui::CoverGridProps props;
  props.itemProvider = &LibraryListActivity::gridItemProvider;
  props.itemProviderUserData = this;
  props.count = static_cast<uint16_t>(count);
  props.topIndex = static_cast<uint16_t>(top);
  props.selectedIndex = tabsFocused() ? -1 : static_cast<int16_t>(selectedEntry());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.titleText = screen.theme().smallText;
  props.titleText.bold = true;
  props.titleText.maxLines = 1;
  props.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  props.columns = g.columns;
  props.coverSize = fui::Size{g.coverW, g.coverH};
  props.rowHeight = g.cellH;
  props.gap = g.gap;
  props.cellInset = fui::Insets{GRID_CELL_PAD / 2, 0, GRID_CELL_PAD / 2, 0};
  props.labelInset = fui::Insets{0, 2, 0, 2};
  props.labelHeight = g.lineH;
  props.labelGap = GRID_LABEL_GAP;
  props.coverPainter = &LibraryListActivity::gridCoverPainter;
  props.coverPainterUserData = this;
  fui::coverGrid(screen.frame(), body, props);

  // Author and series lines under each title, laid out with the grid's own
  // cell arithmetic (columns, gap, scroll indicator reservation).
  const int cellsVisible = g.pageItems();
  const bool overflows = count > cellsVisible;
  int16_t gridW = body.width;
  if (props.scrollIndicator && overflows && props.scrollIndicatorWidth > 0) {
    gridW = static_cast<int16_t>(gridW - props.scrollIndicatorWidth - props.scrollIndicatorGap);
  }
  const int16_t cellW = static_cast<int16_t>((gridW - (g.columns - 1) * g.gap) / g.columns);
  fui::TextStyle line = screen.theme().smallText;
  line.align = fui::TextAlign::Center;
  line.maxLines = 1;
  for (int visible = 0; visible < cellsVisible && top + visible < count; visible++) {
    const int offset = top + visible - gridWindowTop;
    if (gridWindowTop < 0 || offset < 0 || offset >= static_cast<int>(gridCells.size())) continue;
    const GridCell& cell = gridCells[static_cast<size_t>(offset)];
    const int col = visible % g.columns;
    const int row = visible / g.columns;
    const int16_t x = static_cast<int16_t>(body.x + col * (cellW + g.gap) + 2);
    const int16_t width = static_cast<int16_t>(cellW - 4);
    const int16_t coverBottom = static_cast<int16_t>(body.y + row * (g.cellH + g.gap) + props.cellInset.top + g.coverH);
    const int16_t authorY = static_cast<int16_t>(coverBottom + GRID_LABEL_GAP + g.lineH);
    if (!cell.author.empty()) screen.target().text(fui::Rect{x, authorY, width, g.lineH}, cell.author.c_str(), line);
    if (!cell.series.empty()) {
      screen.target().text(fui::Rect{x, static_cast<int16_t>(authorY + g.lineH), width, g.lineH}, cell.series.c_str(),
                           line);
    }
  }
}

void LibraryListActivity::formatInitialHeading(uint32_t initial, std::string& out) {
  out.clear();
  if (initial == 0) {
    out.push_back('#');
    return;
  }
  if (initial >= 'a' && initial <= 'z') initial -= 'a' - 'A';
  utf8AppendCodepoint(initial, out);
}

void LibraryListActivity::formatAuthorHeading(const std::string& author, std::string& out) const {
  out = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
  if (author.empty()) return;
  const size_t lastSpace = out.find_last_of(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < out.size()) {
    out = out.substr(lastSpace + 1) + ", " + out.substr(0, lastSpace);
  }
}

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  fui::HeaderProps header;
  header.title = headerTitle();
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  header.styles = theme.popup;
  if (header.styles.normal.border.kind == fui::PaintKind::None && theme.headerUnderline > 0) {
    header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    header.styles.normal.borderWidth = theme.headerUnderline;
  }
  header.trailingStyles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
  header.leadingStyles = header.trailingStyles;
  header.borderEdges = fui::EdgeBottom;
  // Same battery/clock band as every GUI.drawHeader screen; the header
  // heights are unified across themes, so the buttons derive from the band.
  GUI.applyHeaderStatus(renderer, header);
  if (mappedInput.hasTouch()) {
    header.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    header.leadingAction = ACTION_BACK;
  }
  if (!degraded) {
    // Keep both touch actions together on the right; button boards reach
    // rebuild through the row options menu.
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    if (mappedInput.hasTouch()) {
      header.trailingAdjacentIcon = fui::bitmapFromIcon(gridMode() ? icon_list_32 : icon_layout_grid_32);
      header.trailingAdjacentAction = ACTION_VIEW;
    }
    // Vertical placement comes from applyHeaderStatus: buttons center on the
    // unified band.
  }
  const auto frameRect = screen.frame().screen();
  const bool refreshButton = !degraded && mappedInput.hasTouch();
  const bool queueButton = remoteEnabled && refreshButton && !queueIds.empty();
  // Keep the title clear of the extra trailing buttons.
  if (refreshButton) header.rightReserve = static_cast<int16_t>((header.trailingSize + 4) * (queueButton ? 2 : 1));
  // Header and tabs share a screen-relative boundary, independent of bezel insets.
  const fui::Rect band = BaseTheme::headerBand(renderer,
                                               fui::Rect{frameRect.x, static_cast<int16_t>(metrics.topPadding),
                                                         frameRect.width, static_cast<int16_t>(metrics.headerHeight)},
                                               header);
  fui::header(screen.frame(), band, header);
  const fui::Rect actionRow{band.x, static_cast<int16_t>(band.y + 4 + header.actionOffsetY), band.width,
                            header.trailingSize};
  if (refreshButton) buildRefreshButton(screen, actionRow);
  if (queueButton) buildQueueButton(screen, actionRow);
}

// Left of the refresh button while something is queued: opens the queue.
void LibraryListActivity::buildQueueButton(UiScreen& screen, const fui::Rect& band) {
  const int16_t size = band.height;
  const fui::Rect rect{static_cast<int16_t>(band.right() - 12 - size * 4 - 12), band.y, size, band.height};
  screen.frame().hit(rect, ACTION_QUEUE, 0, fui::InputTouch);
  const fui::BitmapRef icon = fui::bitmapFromIcon(icon_download_24);
  const fui::Rect iconRect{static_cast<int16_t>(rect.x + (rect.width - icon.width) / 2),
                           static_cast<int16_t>(rect.y + (rect.height - icon.height) / 2),
                           static_cast<int16_t>(icon.width), static_cast<int16_t>(icon.height)};
  screen.target().bitmap(iconRect, icon, fui::BitmapMode::Center, fui::Paint::solid(fui::Color::Black));
}

void LibraryListActivity::queueTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openQueue();
}

void LibraryListActivity::openQueue() {
  app.clearTapFlash();
  closeSources();
  auto activity = makeUniqueNoThrow<BookOrbitQueueActivity>(renderer, mappedInput);
  if (!activity) {
    reopenSources();
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    swallowHeldReleases();
    {
      RenderLock lock(*this);
      reopenSources();
      resolvePinned();
    }
    closeRouting();
    requestUpdate(true);
  });
}

// Sits just left of the view toggle. The glyph turns a quarter per repaint
// while the agent runs (pollAgent repaints on a throttle), which on e-ink is
// the honest version of a spinner.
void LibraryListActivity::buildRefreshButton(UiScreen& screen, const fui::Rect& band) {
  const int16_t size = band.height;
  const fui::Rect rect{static_cast<int16_t>(band.right() - 12 - size * 3 - 8), band.y, size, band.height};
  const bool busy = BOOKORBIT_AGENT.available() && BOOKORBIT_AGENT.busy();
  screen.frame().hit(rect, remoteEnabled ? ACTION_REFRESH : ACTION_REBUILD, 0,
                     remoteEnabled ? fui::InputTouch | fui::InputLongPress : fui::InputTouch);
  const fui::BitmapRef icon = fui::bitmapFromIcon(icon_refresh_32);
  const fui::Rect iconRect{static_cast<int16_t>(rect.x + (rect.width - icon.width) / 2),
                           static_cast<int16_t>(rect.y + (rect.height - icon.height) / 2),
                           static_cast<int16_t>(icon.width), static_cast<int16_t>(icon.height)};
  static const fui::Rotation steps[4] = {fui::Rotation::None, fui::Rotation::CW90, fui::Rotation::R180,
                                         fui::Rotation::CCW90};
  const fui::Rotation rotation = busy ? steps[(millis() / 500) % 4] : fui::Rotation::None;
  screen.target().bitmap(iconRect, icon, fui::BitmapMode::Center, fui::Paint::solid(fui::Color::Black), rotation);
}

void LibraryListActivity::refreshTrampoline(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<LibraryListActivity*>(user);
  if (event.longPress)
    self.openOnlineLibrary();
  else
    self.launchRefresh();
}
void LibraryListActivity::openOnlineLibrary() {
  auto browse = makeUniqueNoThrow<BookOrbitBrowseActivity>(renderer, mappedInput);
  if (!browse) return;
  closeSources();
  startActivityForResult(std::move(browse), [this](const ActivityResult&) {
    swallowHeldReleases();
    reopenSources();
    requestUpdate();
  });
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The position readout owns the line above the hints; rows must not overlap
  // it.
  constexpr int READOUT_GAP = 4;  // the list's fold sits just above the readout's line box
  const int16_t readoutReserved = static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID) + READOUT_GAP);
  const Rect area = UITheme::getContentArea(renderer);
  const auto bottomInset = static_cast<int16_t>(renderer.getScreenHeight() - area.y - area.height);
  buildHeader(screen);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                  static_cast<int16_t>(bottomInset + metrics.buttonHintsHeight + readoutReserved), 0});

  if (!degraded) buildTabBar(screen);
  if (listCount() == 0) {
    const char* message = tr(STR_LIBRARY_NO_RESULTS);
    if (filterFailed) {
      message = tr(STR_LIBRARY_SEARCH_UNAVAILABLE);
    } else if (query.empty()) {
      message = remoteEnabled && !remoteStatus.empty() ? remoteStatus.c_str() : tr(STR_LIBRARY_EMPTY);
    }
    screen.centeredText(message);
    return;
  }
  if (gridMode()) {
    buildGrid(screen);
    return;
  }
  buildRows(screen);
}

// "12/69 books" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. How many rows fit varies with the view (author headings
// consume band height), so a page total grows and shrinks as you scroll. The
// book position is stable by construction, and it answers the question the
// reader actually has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() const {
  const int count = listCount();
  if (count <= 0) return;

  char buf[32];
  const char* positionFormat = (groupsCollapsed || catalogGroupMode() || authorGroupMode())
                                   ? tr(STR_LIBRARY_GROUP_POSITION)
                                   : tr(STR_LIBRARY_POSITION);
  snprintf(buf, sizeof(buf), positionFormat, selectedEntry() + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  // Bottom-right of the viewable area, clear of the bezel and rounded corner.
  const Rect area = UITheme::getContentArea(renderer);
  const int right = std::max(renderer.getScreenWidth() - area.x - area.width, bottomCornerClearance());
  const int x = renderer.getScreenWidth() - right - width - SIDE_PADDING;
  const int y = area.y + area.height - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
}

const char* LibraryListActivity::headerTitle() const {
  if (!headerSearchTitle.empty()) return headerSearchTitle.c_str();
  return degraded ? tr(STR_LIBRARY_TITLE_UNSORTED) : tr(STR_LIBRARY);
}

void LibraryListActivity::drawHoldHelp() {
  if (mappedInput.hasTouch() || groupsCollapsed) return;
  const char* help = nullptr;
  const int entry = selectedEntry();
  const int local = localEntryOf(entry);
  if (tabsFocused() && !degraded) {
    help = tr(STR_LIBRARY_HOLD_SORT);
  } else if (!tabsFocused() && isRemoteEntry(entry) && catalogGroupMode()) {
    help = authorTab() ? tr(STR_LIBRARY_HOLD_AUTHOR) : tr(STR_LIBRARY_HOLD_SERIES);
  } else if (!tabsFocused() && isRemoteEntry(entry)) {
    // Only acts on downloaded rows; a flagged conflict takes the hold.
    bookorbit::CatalogRecord record;
    const bool conflict = remoteRecordFor(entry - remoteBase(), record) && files.hasConflict(record.bookId);
    help = conflict ? tr(STR_BOOKORBIT_HOLD_RESOLVE) : tr(STR_BOOKORBIT_HOLD_REMOVE);
  } else if (!tabsFocused() && isRecentSort(sortOrder) && local >= 0 && listCount() > 0) {
    help = tr(STR_LIBRARY_HOLD_OPTIONS);
  } else if (!tabsFocused() && local >= 0 && local < pinnedCount()) {
    help = tr(STR_HOLD_OPEN_TO_REMOVE);  // pinned recents: hold removes from the list
  } else if (!tabsFocused() && local >= 0 && deleteEligible() && listCount() > 0) {
    help = tr(STR_HOLD_OPEN_TO_DELETE);
  } else if (!tabsFocused() && local >= 0 && groupable()) {
    help = tr(STR_LIBRARY_HOLD_GROUPS);
  }
  if (!help) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const Rect area = UITheme::getContentArea(renderer);
  const int left = std::max(area.x, bottomCornerClearance());
  const int y = area.y + area.height - metrics.buttonHintsHeight - lineHeight;
  GUI.drawHelpText(renderer,
                   Rect{left + SIDE_PADDING, y, renderer.getScreenWidth() / 2 - left - SIDE_PADDING, lineHeight}, help);
}

// OptionPopup is a self-contained modal: it owns the whole frame (hints
// included) whenever it is up, mirroring the FileBrowser pattern.
void LibraryListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiTabListActivity::render(std::move(lock));
}

// Grid mode has no section heading to carry the BookOrbit status, so it goes
// to the footer's left slot (the hold help occupies it on button devices).
void LibraryListActivity::drawGridStatus() const {
  if (!gridMode() || !remoteEnabled || !mappedInput.hasTouch()) return;
  std::string status;
  formatAgentStatus(status);
  if (status.empty()) status = remoteStatus;
  if (status.empty()) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight;
  GUI.drawHelpText(renderer, Rect{SIDE_PADDING, y, renderer.getScreenWidth() * 2 / 3 - SIDE_PADDING, lineHeight},
                   status.c_str());
}

void LibraryListActivity::drawFooter() {
  drawPositionReadout();
  drawHoldHelp();
  drawGridStatus();

  const bool backGoesHome =
      tabsFocused() && !groupsCollapsed && query.empty() && openCatalog < 0 && openLocalAuthor < 0;
  const char* backLabel = backGoesHome ? tr(STR_HOME) : tr(STR_BACK);
  const int entry = selectedEntry();
  const char* confirmLabel = groupsCollapsed ? tr(STR_SELECT) : tr(STR_OPEN);
  if (!tabsFocused() && isRemoteEntry(entry) && !catalogGroupMode()) {
    bookorbit::CatalogRecord record;
    if (remoteRecordFor(entry - remoteBase(), record) && !files.isComplete(record.bookId)) {
      confirmLabel = tr(STR_DOWNLOAD);
    }
  }
  const bool canSearch = tabsFocused() && !degraded;
  const auto labels = mappedInput.mapLabels(backLabel, tabsFocused() ? tr(STR_TOGGLE) : confirmLabel,
                                            canSearch ? tr(STR_SEARCH) : tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
