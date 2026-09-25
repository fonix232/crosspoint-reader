#include "BookOrbitSyncActivity.h"

#include <BookOrbitCatalogCache.h>
#include <BookOrbitCredentialStore.h>
#include <BookOrbitPaths.h>
#include <BookOrbitSyncState.h>
#include <FontCacheManager.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "BookOrbitUi.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "bookorbit/BookOrbitAgent.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr char TAG[] = "BOSYNC";

// Book id -> title lookup for the conflict prompt; a linear scan is fine for
// the handful of conflicts a session can hold.
std::string titleForBook(const std::string& account, const uint32_t bookId) {
  bookorbit::CatalogCache cache;
  if (!cache.open(account)) return "";
  const uint16_t ordinal = cache.ordinalForBook(bookId);
  bookorbit::CatalogRecord record;
  if (ordinal == 0xFFFF || !cache.readRecord(ordinal, record)) return "";
  return std::string(record.titleView());
}
}  // namespace

BookOrbitSyncActivity::BookOrbitSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const ReturnTo returnTo, const uint32_t insightsMonthCode)
    : Activity("BookOrbitSync", renderer, mappedInput),
      UiAppHost(renderer),
      returnTo(returnTo),
      insightsMonthCode(insightsMonthCode) {
  conflicts.reserve(bookorbit::Session::MAX_CONFLICTS);
  conflictTitles.reserve(bookorbit::Session::MAX_CONFLICTS);
}

void BookOrbitSyncActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_ROW, &BookOrbitSyncActivity::onRowEvent, this);
  app.setScreen(&BookOrbitSyncActivity::screenFn, this);

  if (!BOOKORBIT_STORE.isConfigured()) {
    state = FAILED;
    statusMessage = tr(STR_BOOKORBIT_ERR_NOT_CONFIGURED);
    requestUpdate();
    return;
  }
  account = BOOKORBIT_STORE.getAccountKey();
  BOOKORBIT_AGENT.pause();
  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  state = WIFI_SELECTION;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookOrbitSyncActivity::onExit() {
  Activity::onExit();
  client.end();
  BOOKORBIT_AGENT.resume();
  if (wifiActivated && !BOOKORBIT_AGENT.wifiHeld() && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    restartToOrigin();
  }
}

void BookOrbitSyncActivity::restartToOrigin() {
  switch (returnTo) {
    case ReturnTo::Library:
      silentRestartToLibrary();
      break;
    case ReturnTo::Insights:
      silentRestartToBookOrbitInsights();
      break;
    default:
      silentRestartToBookOrbitSettings();
      break;
  }
}

void BookOrbitSyncActivity::leave() {
  client.end();
  if (BOOKORBIT_AGENT.wifiHeld()) {
    // The agent keeps the link; no heap-defrag reboot needed to leave.
    switch (returnTo) {
      case ReturnTo::Library:
        activityManager.goToLibrary();
        break;
      case ReturnTo::Insights:
        activityManager.goToBookOrbitInsights();
        break;
      default:
        activityManager.goToBookOrbitSettings();
        break;
    }
    return;
  }
  if (wifiActivated && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  restartToOrigin();
  // Only reached when a deep sleep is in progress and the restart was skipped.
  onGoHome();
}

void BookOrbitSyncActivity::setStatus(const char* status, const char* detail) {
  {
    RenderLock lock(*this);
    statusMessage = status ? status : "";
    detailMessage = detail ? detail : "";
    totalItems = completedItems = 0;
    readingEvents[0] = 0;
  }
  requestUpdateAndWait();
}

// Pumps input between steps so a Back press or home gesture can stop the
// session at the next boundary. Returns true when cancellation was requested.
bool BookOrbitSyncActivity::pumpCancel() {
  mappedInput.update(true);
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
    cancelRequested = true;
  }
  return cancelRequested;
}

bool BookOrbitSyncActivity::shouldCancel(void* ctx) { return static_cast<BookOrbitSyncActivity*>(ctx)->pumpCancel(); }

void BookOrbitSyncActivity::updateProgress(const bookorbit::SessionPhase phase, const uint32_t a, const uint32_t b) {
  if (phase == bookorbit::SessionPhase::ReadingEvents) {
    snprintf(readingEvents, sizeof(readingEvents), tr(STR_BOOKORBIT_EVENT_COUNTS), static_cast<unsigned>(a),
             static_cast<unsigned>(b));
    return;
  }
  displayedPhase = phase;
  completedItems = std::min(a, b);
  totalItems = b;
  readingEvents[0] = 0;
  switch (phase) {
    case bookorbit::SessionPhase::Catalog:
      detailMessage = tr(STR_BOOKORBIT_CATALOG_PROGRESS);
      break;
    case bookorbit::SessionPhase::Covers:
      detailMessage = tr(STR_BOOKORBIT_COVER_PROGRESS);
      break;
    case bookorbit::SessionPhase::Progress:
      detailMessage = tr(STR_BOOKORBIT_BOOK_PROGRESS);
      break;
    case bookorbit::SessionPhase::Matching:
      detailMessage = tr(STR_BOOKORBIT_MATCHING_PROGRESS);
      break;
    case bookorbit::SessionPhase::Insights:
      detailMessage = tr(STR_BOOKORBIT_FETCHING_INSIGHTS);
      break;
    default:
      break;
  }
}

void BookOrbitSyncActivity::onPhase(void* ctx, const bookorbit::SessionPhase phase, const uint32_t a,
                                    const uint32_t b) {
  auto* self = static_cast<BookOrbitSyncActivity*>(ctx);
  if (phase != bookorbit::SessionPhase::Catalog && phase != bookorbit::SessionPhase::Covers &&
      phase != bookorbit::SessionPhase::Progress && phase != bookorbit::SessionPhase::Matching &&
      phase != bookorbit::SessionPhase::ReadingEvents && phase != bookorbit::SessionPhase::Insights)
    return;
  const bool changed = phase != bookorbit::SessionPhase::ReadingEvents && phase != self->displayedPhase;
  const bool finished = phase == bookorbit::SessionPhase::ReadingEvents ? b == 0 : b > 0 && a >= b;
  const bool repaint = changed || finished || millis() - self->lastProgressPaint >= 1000;
  if (self->progressRenderLocked) {
    // This callback runs between mappers, with the session's render lock held.
    // Paint directly: waiting for the render task here would deadlock.
    self->updateProgress(phase, a, b);
    if (repaint) self->drawScreen();
  } else {
    {
      RenderLock lock(*self);
      self->updateProgress(phase, a, b);
    }
    if (repaint) self->requestUpdateAndWait();
  }
  if (repaint) self->lastProgressPaint = millis();
}

void BookOrbitSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      statusMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }
  WiFi.setSleep(false);
  {
    RenderLock lock(*this);
    state = RUNNING;
    statusMessage = tr(STR_BOOKORBIT_SYNCING);
  }
  requestUpdateAndWait();
  runSession();
}

void BookOrbitSyncActivity::runSession() {
  sessionStart = bookorbit::nowIfClockValid();
  // Rebuildable SD-font caches can hold tens of KB the TLS session needs.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();

  bookorbit::SessionCallbacks callbacks{this, &BookOrbitSyncActivity::onPhase, &BookOrbitSyncActivity::shouldCancel};
  bookorbit::Session session(client, files, renderer, account, /*lendFramebuffer=*/true, callbacks);
  bookorbit::ClientError error = session.connect();
  if (error != bookorbit::ClientError::OK) {
    finishSession(false, error);
    return;
  }
  error = session.refreshCatalog(sessionStart, catalogResult);
  if (error != bookorbit::ClientError::OK) {
    finishSession(false, error);
    return;
  }
  if (pumpCancel()) {
    finishSession(true, bookorbit::ClientError::ABORTED);
    return;
  }
  session.refreshCovers();
  if (pumpCancel()) {
    finishSession(true, bookorbit::ClientError::ABORTED);
    return;
  }

  if (BOOKORBIT_STORE.getSyncProgress()) {
    setStatus(tr(STR_BOOKORBIT_SYNCING), tr(STR_BOOKORBIT_SYNCING_PROGRESS));
    // Locator mapping borrows the framebuffer; no rendering may run meanwhile.
    RenderLock lock(*this);
    progressAttempted = true;
    progressRenderLocked = true;
    progressStats = session.syncProgress(bookorbit::ReaderSnapshot{}, /*askOnConflict=*/true, conflicts);
    progressRenderLocked = false;
  }
  if (pumpCancel()) {
    finishSession(true, bookorbit::ClientError::ABORTED);
    return;
  }
  setStatus(tr(STR_BOOKORBIT_SYNCING), tr(STR_BOOKORBIT_FETCHING_INSIGHTS));
  insightsError = session.refreshInsights(static_cast<uint16_t>(insightsMonthCode / 100),
                                          static_cast<uint8_t>(insightsMonthCode % 100));
  insightsAttempted = true;

  if (!conflicts.empty()) {
    conflictTitles.clear();
    for (const auto& conflict : conflicts) conflictTitles.push_back(titleForBook(account, conflict.bookId));
    conflictIndex = 0;
    selectedOption = 0;
    {
      RenderLock lock(*this);
      state = CONFLICT;
    }
    requestUpdate();
    return;
  }
  finishSession(true, bookorbit::ClientError::OK);
}

void BookOrbitSyncActivity::resolveConflict(const bool keepLocal) {
  if (conflictIndex >= conflicts.size()) return;
  {
    RenderLock lock(*this);
    state = RUNNING;
    statusMessage = tr(STR_BOOKORBIT_SYNCING_PROGRESS);
    detailMessage = conflictTitles[conflictIndex];
  }
  requestUpdateAndWait();
  bookorbit::SessionCallbacks callbacks{this, nullptr, &BookOrbitSyncActivity::shouldCancel};
  bookorbit::Session session(client, files, renderer, account, /*lendFramebuffer=*/true, callbacks);
  bool ok;
  {
    RenderLock lock(*this);
    ok = session.resolveConflict(conflicts[conflictIndex], keepLocal);
  }
  if (ok) {
    if (keepLocal)
      progressStats.uploaded++;
    else
      progressStats.applied++;
  } else {
    progressStats.errors++;
  }
  conflictIndex++;
  if (conflictIndex < conflicts.size()) {
    selectedOption = 0;
    {
      RenderLock lock(*this);
      state = CONFLICT;
    }
    closeRouting();
    requestUpdate();
    return;
  }
  finishSession(true, bookorbit::ClientError::OK);
}

void BookOrbitSyncActivity::finishSession(bool ok, bookorbit::ClientError error) {
  if (ok && error == bookorbit::ClientError::OK && progressStats.errors > 0) {
    ok = false;
    error = bookorbit::ClientError::SERVER;
  }
  client.end();
  bookorbit::SyncState syncState;
  bookorbit::loadSyncState(account, syncState);
  syncState.lastAttemptAt = sessionStart;
  syncState.lastOk = ok;
  syncState.lastErrorCode = static_cast<uint8_t>(error);
  if (ok) {
    syncState.lastSuccessAt = sessionStart;
    syncState.serverTotal = catalogResult.serverTotal;
    syncState.cachedBooks = catalogResult.cached;
  }
  if (progressAttempted) {
    syncState.progressAttemptAt = bookorbit::nowIfClockValid();
    syncState.progressAttemptOk = ok && error == bookorbit::ClientError::OK && progressStats.errors == 0;
  }
  if (insightsAttempted) bookorbit::Session::noteInsightsResult(syncState, insightsError, sessionStart);
  if (!account.empty()) bookorbit::saveSyncState(account, syncState);
  BOOKORBIT_AGENT.noteForegroundResult(error);
  LOG_INF(TAG, "session %s: %u books, %u uploaded, %u applied, %u errors (%s)", ok ? "ok" : "failed",
          catalogResult.cached, progressStats.uploaded, progressStats.applied, progressStats.errors,
          bookorbit::clientErrorName(error));

  // Drop the radio while the result is shown unless the agent keeps it; the
  // reboot (or the agent's idle timer) does the teardown.
  if (wifiActivated && !BOOKORBIT_AGENT.wifiHeld() && WiFi.getMode() != WIFI_MODE_NULL) WiFi.disconnect(false);
  {
    RenderLock lock(*this);
    state = ok ? DONE : FAILED;
    statusMessage = ok ? tr(STR_BOOKORBIT_SYNC_DONE) : tr(STR_BOOKORBIT_SYNC_FAILED);
    detailMessage = ok ? (error == bookorbit::ClientError::ABORTED ? tr(STR_BOOKORBIT_ERR_CANCELLED) : "")
                       : bookOrbitErrorText(error);
  }
  if (ok) autoReturnAt = millis() + AUTO_RETURN_MS;
  requestUpdate();
}

void BookOrbitSyncActivity::screenFn(UiScreen& screen, void* user) {
  auto* self = static_cast<BookOrbitSyncActivity*>(user);
  if (self->state == CONFLICT) self->buildConflictScreen(screen);
}

void BookOrbitSyncActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitSyncActivity*>(user);
  if (self->state != CONFLICT || event.value < 0 || event.value > 1) return;
  self->app.clearTapFlash();
  self->selectedOption = event.value;
  self->resolveConflict(self->selectedOption == 0);
}

void BookOrbitSyncActivity::buildConflictScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (conflictIndex >= conflicts.size()) return;
  const auto& conflict = conflicts[conflictIndex];
  const std::string& title = conflictTitles[conflictIndex];

  auto titleStyle = screen.theme().bodyText;
  titleStyle.bold = true;
  titleStyle.maxLines = 2;
  const int16_t indent = static_cast<int16_t>(screen.theme().listInset + screen.theme().listSidePadding);
  fui::Rect r =
      screen.takeTop(static_cast<int16_t>(screen.target().lineHeight(titleStyle.font) * 2), screen.theme().spaceMd);
  r.x = static_cast<int16_t>(r.x + indent);
  r.width = static_cast<int16_t>(r.width - indent * 2);
  screen.target().text(r, title.empty() ? tr(STR_BOOKORBIT_CONFLICT_TITLE) : title.c_str(), titleStyle);

  char keepLabel[64];
  char useLabel[64];
  snprintf(keepLabel, sizeof(keepLabel), tr(STR_BOOKORBIT_KEEP_DEVICE), static_cast<int>(conflict.localPct * 100));
  snprintf(useLabel, sizeof(useLabel), tr(STR_BOOKORBIT_USE_SERVER),
           static_cast<int>(conflict.remote.percentage * 100));
  fui::ListItem actions[2];
  actions[0].label = keepLabel;
  actions[0].icon = fui::bitmapFromIcon(icon_upload_24);
  actions[0].actionValue = 0;
  actions[1].label = useLabel;
  actions[1].icon = fui::bitmapFromIcon(icon_download_24);
  actions[1].actionValue = 1;
  fui::ListProps props;
  props.items = actions;
  props.count = 2;
  props.selectedIndex = static_cast<int16_t>(selectedOption);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.scrollIndicator = false;
  int16_t rowHeight = screen.theme().rowHeight;
  if (!mappedInput.hasTouch()) {
    rowHeight = static_cast<int16_t>(metrics.listRowHeight);
    props.rowHeight = rowHeight;
  }
  const auto band = static_cast<int16_t>(rowHeight * 2 + screen.theme().listRowGap + screen.theme().spaceSm);
  screen.list(props, band);
}

void BookOrbitSyncActivity::render(RenderLock&&) { drawScreen(); }

void BookOrbitSyncActivity::drawScreen() {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 state == CONFLICT ? tr(STR_BOOKORBIT_CONFLICT_TITLE) : tr(STR_BOOKORBIT));
  const int top = screen.y + screen.height / 2 - (state == RUNNING ? 90 : 40);

  if (state == CONFLICT) {
    renderUi();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
  if (!detailMessage.empty()) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, detailMessage.c_str());
  }
  if (state == RUNNING && totalItems > 0) {
    const int width = screen.width * 3 / 4;
    GUI.drawProgressBar(renderer, Rect{screen.x + (screen.width - width) / 2, top + 80, width, 16}, completedItems,
                        totalItems);
    char counts[80];
    snprintf(counts, sizeof(counts), tr(STR_BOOKORBIT_SYNC_COUNTS), static_cast<unsigned>(completedItems),
             static_cast<unsigned>(totalItems), static_cast<unsigned>(totalItems - completedItems));
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 110, counts);
  }
  if (state == RUNNING && readingEvents[0]) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 150, readingEvents);
  }
  const char* backLabel = state == RUNNING ? tr(STR_CANCEL) : tr(STR_BACK);
  const auto labels = mappedInput.mapLabels(backLabel, state == DONE || state == FAILED ? tr(STR_DONE) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BookOrbitSyncActivity::loop() {
  if (state == DONE || state == FAILED) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      leave();
      return;
    }
    int x = 0;
    int y = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      leave();
    }
    return;
  }
  if (state == CONFLICT) {
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      resolveConflict(selectedOption == 0);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // Unresolved conflicts keep both positions and are asked again next time.
      finishSession(true, bookorbit::ClientError::OK);
    }
  }
}
