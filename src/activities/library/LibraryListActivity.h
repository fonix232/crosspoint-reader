#pragma once

#include <BookOrbitCatalogCache.h>
#include <BookOrbitFileStore.h>
#include <BookOrbitSyncState.h>
#include <LibraryIndexFile.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"

// One Library screen: every indexed book on the card shown by recency, title,
// or author. The Recent shelf orders by file modification time (when a book
// landed on the card) and pins the recently OPENED books from RecentBooksStore
// on top, so active reads and fresh arrivals share one list.
//
// The two-slot row is the whole point rather than a styling choice: the problem
// being solved is "I cannot find my books because I do not know the authors",
// and that is answered by a column the eye can sweep, not by a tidier filename.
//
// Rows render through fui::list on the UiTabListActivity ring (0 = the sort
// strip, 1..N = the books), which is what brings touch to rows and tabs. Titles
// are truncated to one line by the widget — more books on the screen, even if
// half a name is hidden.
//
// Only the visible window of rows is materialized per render (strings and
// ListItems for at most one page). The ordinary shelf therefore keeps one page
// of strings; an active search additionally uses one fallible uint16_t slot per
// indexed book so an allocation failure remains recoverable on the C3.
//
// With a BookOrbit account configured the shelf gains a second source. The
// three sort tabs show everything: a "BookOrbit" section (the SD catalogue
// cache, headed by a Refresh row that runs the network session) followed by
// an "Only on this device" section (the CLIX index). A fourth "On device" tab
// shows what can be opened offline: downloaded BookOrbit books plus sideloaded
// files, by title. Managed downloads never enter CLIX; their mapping index is
// the local source for them. Entries are laid out
// [refresh row][remote rows][local rows], and every local-only helper below
// takes a LOCAL entry (0-based within its section).
class LibraryListActivity final : public UiTabListActivity {
 public:
  LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  // Runs the base input loop, then prepares the grid page the selection landed
  // on before the deferred render draws it.
  void loop() override;

 protected:
  // --- UiListActivity / UiTabListActivity contract ---------------------------
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
#if FREEINK_CAP_TOUCH
  bool canSwipeDelete(int index) const override { return !groupsCollapsed && index >= 0 && index < listCount(); }
  void swipeDelete(int index) override;
#endif
  int tabCount() const override;
  int activeTab() const override;
  const char* tabLabel(int index) const override;
  freeink::ui::BitmapRef tabIcon(int index) const override;
  freeink::ui::TabIndicator tabIndicator(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void navigateButtons() override;
  // The FreeInkUI header owns both the title and search touch target.
  void drawChrome() override {}
  void drawFooter() override;
  // OptionPopup is a self-contained modal: it owns rendering (and the button
  // hints) whenever it is up.
  void render(RenderLock&& lock) override;

 private:
  // The screen's own actions, after the base's ACTION_ROW / ACTION_TAB.
  static constexpr freeink::ui::ActionId ACTION_SEARCH = ACTION_TAB_USER;
  static constexpr freeink::ui::ActionId ACTION_REBUILD = ACTION_SEARCH + 1;
  static constexpr freeink::ui::ActionId ACTION_BACK = ACTION_REBUILD + 1;

  // Walk the card and write a fresh index. Blocking, with a popup: at ~70 books
  // it is well under a second, and it only runs when the index is missing or the
  // user asks.
  bool rebuildIndex();

  // Input
  void openSelectedBook();
  void openLocalEntry(int localEntry);
  void openOnlineLibrary();
  void openSearch();
  // Shared tail of row activation and the options menu's Open entry.
  void openBookByPath(const std::string& path);
  void promptRebuildIndex();
  void resetAfterRebuild();
  // Recent-row long-press menu: open / remove from recents / delete / rebuild.
  void showRecentBookOptions(int entry);
  void promptRemoveRecentBook(const std::string& path, const std::string& title);
  // Long-press delete owns the gesture where grouping does not apply: the
  // Recent sort, degraded lists, and any active search result.
  bool deleteEligible() const;
  // Resolves the row's path and title, then confirms via promptDeleteBookByPath.
  void promptDeleteBook(int localEntry);
  void promptDeleteBookByPath(const std::string& path, const std::string& title);
  bool collapseGroups(int bookEntry);
  void expandGroup(int groupEntry);
  void restoreExpandedList();
  void selectTab(int index, bool toggleIfActive);
  void toggleSortDirection();
  // Sub-screens act on button press, so a button still held when we resume must
  // not also act here. Records what to swallow on the next release.
  void swallowHeldReleases();
  // Staged back-out shared by Button::Back and the header back arrow.
  void handleBackAction();
  static void searchActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void rebuildActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void backActionTrampoline(const freeink::ui::ActionEvent& event, void* user);

  // Data
  void applyFilter();
  int bookRowCount() const;
  int rowFor(int localEntry) const;
  // fileName, when asked for, is the on-card name the row's icon derives from
  // (the display title may come from metadata and carry no extension).
  bool rowTextFor(int localEntry, std::string& title, std::string& author, std::string* fileName = nullptr);
  uint32_t titleInitialFor(int localEntry);
  bool buildGroupStarts();
  int groupForBook(int bookEntry) const;
  bool groupable() const;

  // Screen building
  void buildHeader(UiScreen& screen);
  // Materializes ListItems and their strings for the visible window only.
  void buildRows(UiScreen& screen);
  static void formatInitialHeading(uint32_t initial, std::string& out);
  void formatAuthorHeading(const std::string& author, std::string& out) const;
  void drawPositionReadout() const;
  void drawHoldHelp();
  const char* headerTitle() const override;

  // Ring 0 is the strip; the selected entry is ring - 1, with the strip keeping
  // row 0 as the working selection exactly as the pre-ring code did.
  int selectedEntry() const;
  bool tabsFocused() const { return ringPos() == 0; }

  // --- pinned recently-opened overlay ---------------------------------------
  // On the unfiltered Recent shelf the RecentBooksStore entries sit on top, in
  // read order; the modification-time list follows with those books skipped.
  // Local entries below pinnedCount() are store rows; the rest go through
  // rowFor().
  int pinnedCount() const;
  // The store entry behind a pinned slot (managed downloads are skipped when
  // BookOrbit shows them in its own section), or null when out of range.
  const RecentBook* pinnedBook(int slot) const;
  // Re-match the store against the index (chunked scan). Call whenever the
  // index or the store changes.
  void resolvePinned();
  // Direction-space translation of the matched rows. Call on sort toggles.
  void refreshOverlap();

  // --- BookOrbit section -----------------------------------------------------
  bool remoteActive() const { return remoteEnabled && catalog.isOpen(); }
  bool deviceTab() const;
  // --- Grouped views --------------------------------------------------------
  // Series groups use the catalogue only; Author groups retain the separate
  // catalogue and local sections. Opening either shows only that group's books.
  bool seriesTab() const;
  bool authorTab() const;
  bool authorGroupMode() const;
  void openLocalAuthorGroup(int group);
  void closeLocalAuthorGroup();
  bool catalogGroupMode() const;
  void buildCatalogGroups();
  void openCatalogGroup(int group);
  void closeCatalogGroup();
  // Hold on a catalogue group: queue its books that are not on the card.
  void promptDownloadGroup(int group);
  // Entry layout: [refresh][remote][local].
  int refreshRowCount() const;
  int remoteRowCount() const;
  int remoteBase() const { return refreshRowCount(); }
  int localBase() const { return remoteBase() + remoteRowCount(); }
  bool isRemoteEntry(int entry) const { return entry >= remoteBase() && entry < localBase(); }
  int localEntryOf(int entry) const { return entry - localBase(); }
  // Ascending row in the catalogue's active order for a remote entry, and the
  // record behind it.
  int remotePositionFor(int remoteEntry) const;
  uint16_t remoteRowFor(int remoteEntry) const;
  bool remoteRecordFor(int remoteEntry, bookorbit::CatalogRecord& out);
  void selectRemoteOrder();
  void applyRemoteFilter();
  void openRemoteEntry(int remoteEntry);
  void launchRefresh();
  // Header toolbar: the BookOrbit refresh button (touch devices), spinning
  // while the background agent works.
  void buildRefreshButton(UiScreen& screen, const freeink::ui::Rect& band);
  static void refreshTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static constexpr freeink::ui::ActionId ACTION_REFRESH = ACTION_TAB_USER + 4;
  void promptRemoveManaged(uint32_t bookId, const std::string& title);
  // Download queue membership (queued, downloading, or failed) for row badges
  // and the On device tab.
  void reloadQueue();
  bool queuedBook(uint32_t bookId) const;
  bool queueFailed(uint32_t bookId) const;
  void openQueue();
  void buildQueueButton(UiScreen& screen, const freeink::ui::Rect& band);
  static void queueTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static constexpr freeink::ui::ActionId ACTION_QUEUE = ACTION_TAB_USER + 5;
  void launchConflictResolver(uint32_t bookId, std::string title);
  void formatRemoteStatus(std::string& out) const;
  // Live text for the Refresh row while the background agent works.
  void formatAgentStatus(std::string& out) const;
  bool loadCoverTile(uint32_t bookId, uint8_t* tile) const;
  void closeSources();
  void reopenSources();
  // Follows the background agent: releases the catalogue while it is being
  // replaced, reloads after a change, and repaints for live row states.
  void pollAgent();

  // --- Grid view ---------------------------------------------------------------
  // Kindle-style cover grid. Cell size follows the panel's physical density
  // (screen_metrics::ppi) so covers stay about the same width in millimetres
  // on every device; columns and rows then follow from the resolution. The
  // page holding the selection is prepared on the loop task (thumbnails,
  // series) before the render task draws it, so drawing never decodes images.
  struct GridGeometry {
    uint8_t columns = 1;
    uint16_t rows = 1;
    int16_t coverW = 0;
    int16_t coverH = 0;
    int16_t cellH = 0;
    int16_t gap = 0;
    int16_t lineH = 0;
    int pageItems() const { return columns * rows; }
  };
  struct GridCell {
    int entry = -1;
    std::string title;
    std::string author;
    std::string series;
    std::string thumbPath;         // 1-bit BMP, empty when the book has no cover
    std::string bookPath;          // local book on the card (empty for BookOrbit rows)
    bool remoteOnly = false;       // BookOrbit book not yet on the card
    bool generationTried = false;  // a missing thumbnail was attempted this visit
  };
  bool gridMode() const;
  GridGeometry gridGeometry(int bodyWidth, int bodyHeight) const;
  GridGeometry currentGridGeometry() const;
  int gridPageTop(int entry, const GridGeometry& g) const;
  // `renderLocked`: the caller already holds the (non-recursive) render lock.
  void prepareGridWindow(bool renderLocked = false);
  // True when the prepared page changed (the render should follow).
  bool prepareGridWindowChanged(bool renderLocked = false);
  void buildGrid(UiScreen& screen);
  void toggleViewMode();
  bool ensureLocalCover(const std::string& epubPath, GridCell& cell, bool generate);
  bool ensureRemoteCover(uint32_t bookId, GridCell& cell, bool generate);
  static void viewToggleTrampoline(const freeink::ui::ActionEvent& event, void* user);
  void drawGridStatus() const;
  static freeink::ui::CoverGridItem gridItemProvider(uint16_t index, void* user);
  static bool gridCoverPainter(freeink::ui::DrawTarget& target, freeink::ui::Rect rect,
                               const freeink::ui::CoverGridItem& item, uint16_t index, void* user);
  static constexpr freeink::ui::ActionId ACTION_VIEW = ACTION_TAB_USER + 3;
  // Missing thumbnails are generated one per loop pass after the page is on
  // screen (generateGridCover): at most this many per page visit, repainting
  // every GRID_REPAINT_EVERY covers and when the page is complete.
  static constexpr int GRID_MAX_GENERATE = 16;
  static constexpr int GRID_REPAINT_EVERY = 4;
  bool generateGridCover();

  std::vector<GridCell> gridCells;  // the prepared page
  int gridWindowTop = -1;
  int gridWindowCount = 0;
  int gridGenerated = 0;  // thumbnails attempted for the prepared page
  int gridGenDirty = 0;   // generated since the last repaint
  // Body measured by the last grid render; 0 until then (an estimate is used).
  int gridBodyW = 0;
  int gridBodyH = 0;
  std::atomic<bool> gridNeedsPrepare{false};

  library::LibraryIndexFile index;
  int activeTabIndex = 0;
  library::SortOrder sortOrder = library::SortOrder::RecentDesc;
  // One bit per tab; Recent starts descending (newest first).
  uint8_t descendingTabs = 1u << 0;
  // Set when the walk finished but the sort did not, so the screen can say the
  // order is discovery order rather than silently showing a wrong one.
  bool degraded = false;

  // Rows surviving the current query, as positions in the active sort order.
  // Empty query means no filtering and this owns no allocation, so the ordinary
  // shelf pays nothing proportional to the library for the feature.
  std::string query;
  // The active query, pre-quoted for the header: headerTitle() returns a
  // stable c_str the render task can hold across a build.
  std::string headerSearchTitle;
  std::unique_ptr<uint16_t[]> filtered;
  uint16_t filteredCount = 0;
  bool filterFailed = false;

  // One start row per group. Grouping is only offered for the sorted <=512-book
  // index, so this fallible allocation is at most 1 KiB and is reused after its
  // first successful allocation.
  std::unique_ptr<uint16_t[]> groupStarts;
  uint16_t groupCapacity = 0;
  uint16_t groupCount = 0;
  bool groupsCollapsed = false;
  freeink::ui::ListNav expandedNav;

  // Visible-window row storage, reused across renders (buildRows). Bounded by
  // the densest page, never by the library. Headings get their own storage:
  // the surname-first inversion must not overwrite the author slot, whose raw
  // value the next row's group comparison reads.
  std::vector<freeink::ui::ListItem> winItems;
  std::vector<std::string> winTitles;
  std::vector<std::string> winAuthors;
  std::vector<std::string> winHeaders;
  // One cover tile per visible remote row, loaded from the SD tile cache.
  std::unique_ptr<uint8_t[]> winTiles;
  size_t winTilesCap = 0;
  std::string remoteStatus;
  std::string remoteHeading;  // "BookOrbit · <status>" section heading

  // Pinned overlay state: per pinned slot its store index and RecentAsc row
  // (0xFFFF when the book is not in the index), and the current-direction rows
  // to skip, sorted ascending, so unpinned entries map to sort rows with a
  // <=10-step walk.
  uint8_t pinnedStore[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint16_t pinnedAscRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint16_t overlapRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint8_t pinnedTotal = 0;
  uint8_t overlapCount = 0;

  // BookOrbit state. remoteRows holds the surviving catalogue rows when a
  // query or the On device tab narrows the section; otherwise the section is
  // the whole cache and owns no allocation.
  bool remoteEnabled = false;
  std::string account;
  bookorbit::CatalogCache catalog;
  bookorbit::FileStore files;
  bookorbit::SyncState syncState;
  std::unique_ptr<uint16_t[]> remoteRows;
  uint16_t remoteRowsCount = 0;
  bool remoteIdentity = true;
  uint32_t seenAgentGeneration = 0;
  bool agentWasBusy = false;
  unsigned long lastAgentPaintMs = 0;
  std::string agentStatus;
  std::vector<uint32_t> queueIds;
  std::vector<uint32_t> queueFailedIds;
  // Group boundaries in the active catalogue order and the opened group.
  std::unique_ptr<uint16_t[]> catalogGroupStart;
  std::unique_ptr<uint16_t[]> catalogGroupSize;
  uint16_t catalogGroupCapacity = 0;
  uint16_t catalogGroupCount = 0;
  int openLocalAuthor = -1;
  int localAuthorStart = 0;
  int localAuthorSize = -1;
  int openCatalog = -1;

  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;

  // Row options modal (Recent long-press menu); owned here so it outlives the
  // touch event that opened it.
  OptionPopup optionPopup;
};
