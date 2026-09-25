#include "BookOrbitDownloadActivity.h"

#include <BookOrbitCatalogCache.h>
#include <BookOrbitCredentialStore.h>
#include <BookOrbitDownloadQueue.h>
#include <BookOrbitPaths.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "BookOrbitUi.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "bookorbit/BookOrbitAgent.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr char TAG[] = "BODL";
}  // namespace

BookOrbitDownloadActivity::BookOrbitDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const uint32_t bookId, std::string title)
    : Activity("BookOrbitDownload", renderer, mappedInput),
      UiAppHost(renderer),
      bookId(bookId),
      title(std::move(title)) {}

void BookOrbitDownloadActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_CANCEL, &BookOrbitDownloadActivity::onCancelEvent, this);
  app.setScreen(&BookOrbitDownloadActivity::screenFn, this);

  if (!BOOKORBIT_STORE.isConfigured()) {
    fail(tr(STR_BOOKORBIT_ERR_NOT_CONFIGURED));
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

void BookOrbitDownloadActivity::onExit() {
  Activity::onExit();
  client.end();
  BOOKORBIT_AGENT.resume();
  if (wifiActivated && !BOOKORBIT_AGENT.wifiHeld() && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToLibrary();
  }
}

void BookOrbitDownloadActivity::setState(const State next) {
  {
    RenderLock lock(*this);
    state = next;
  }
  requestUpdateAndWait();
}

void BookOrbitDownloadActivity::fail(const char* message) {
  client.end();
  {
    RenderLock lock(*this);
    state = FAILED;
    errorMessage = message ? message : "";
  }
  requestUpdate();
}

void BookOrbitDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    fail(tr(STR_WIFI_CONN_FAILED));
    return;
  }
  WiFi.setSleep(false);
  setState(PREPARING);
  if (bookId == QUEUE) {
    runQueue();
  } else {
    runDownload();
  }
}

// Drains the queue file front to back like the agent does, one connection
// for all of them; a book that fails stays in the queue marked for the queue
// screen.
void BookOrbitDownloadActivity::runQueue() {
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  cancelDownload = false;
  goHomeAfterCancel = false;
  bookorbit::SessionCallbacks callbacks{this, &BookOrbitDownloadActivity::onPhase,
                                        &BookOrbitDownloadActivity::shouldCancel};
  bookorbit::Session session(client, files, renderer, account, /*lendFramebuffer=*/true, callbacks);
  bookorbit::ClientError error = session.connect();
  if (error != bookorbit::ClientError::OK) {
    fail(error == bookorbit::ClientError::FILE ? tr(STR_BOOKORBIT_ERR_STORAGE) : bookOrbitErrorText(error));
    return;
  }
  bookorbit::DownloadQueue queue;
  queue.load(account);
  queueTotal = static_cast<uint16_t>(queue.queuedCount());
  queueDone = 0;
  bookorbit::CatalogCache cache;
  const bool haveCatalog = cache.open(account);
  uint32_t next = 0;
  while (queue.nextQueued(next)) {
    if (haveCatalog) {
      const uint16_t ordinal = cache.ordinalForBook(next);
      bookorbit::CatalogRecord record;
      title = ordinal != 0xFFFF && cache.readRecord(ordinal, record) ? std::string(record.titleView()) : "";
    }
    lastRenderedPercent = -1;
    downloadProgress = 0;
    downloadTotal = 0;
    setState(PREPARING);
    bookorbit::ManagedFile record;
    std::string finalPath;
    error = session.downloadBook(next, record, finalPath);
    if (error == bookorbit::ClientError::ABORTED) break;
    if (error == bookorbit::ClientError::OK) {
      if (BOOKORBIT_STORE.getSyncProgress()) {
        RenderLock lock(*this);
        session.checkIn(record, finalPath);
      }
      queue.remove(next);
      queueDone++;
    } else {
      queue.markFailed(next, static_cast<uint8_t>(error));
    }
    queue.save();
    if (error == bookorbit::ClientError::AUTH || error == bookorbit::ClientError::FORBIDDEN ||
        error == bookorbit::ClientError::NETWORK || error == bookorbit::ClientError::TLS_FAILED ||
        error == bookorbit::ClientError::LOW_MEMORY) {
      fail(bookOrbitErrorText(error));
      return;
    }
  }
  if (error == bookorbit::ClientError::ABORTED && goHomeAfterCancel) {
    onGoHome();
    return;
  }
  leaveToLibrary();
}

// The loop is blocked for the whole transfer; pump input here so Cancel,
// Back, or the home gesture can abort mid-stream.
bool BookOrbitDownloadActivity::shouldCancel(void* ctx) {
  auto* self = static_cast<BookOrbitDownloadActivity*>(ctx);
  self->mappedInput.update(true);
  if (self->mappedInput.wasReleased(MappedInputManager::Button::Back)) self->cancelDownload = true;
  if (self->mappedInput.wasHomeGesture()) {
    self->cancelDownload = true;
    self->goHomeAfterCancel = true;
  }
  self->routeTouch(self->mappedInput);
  return self->cancelDownload;
}

void BookOrbitDownloadActivity::onPhase(void* ctx, const bookorbit::SessionPhase phase, const uint32_t a,
                                        const uint32_t b) {
  auto* self = static_cast<BookOrbitDownloadActivity*>(ctx);
  switch (phase) {
    case bookorbit::SessionPhase::Download: {
      if (self->state != DOWNLOADING) self->setState(DOWNLOADING);
      self->downloadProgress = a;
      self->downloadTotal = b;
      const int percent = b > 0 ? static_cast<int>(static_cast<uint64_t>(a) * 100 / b) : 0;
      const unsigned long now = millis();
      if (percent >= 100 || self->lastRenderedPercent < 0 ||
          percent >= self->lastRenderedPercent + PROGRESS_STEP_PERCENT ||
          now - self->lastProgressUpdateMs >= PROGRESS_MIN_UPDATE_MS) {
        self->lastRenderedPercent = percent;
        self->lastProgressUpdateMs = now;
        self->requestUpdate(true);
      }
      break;
    }
    case bookorbit::SessionPhase::Verify:
      self->setState(VERIFYING);
      break;
    case bookorbit::SessionPhase::Publish:
      self->setState(OPENING);
      break;
    default:
      break;
  }
}

void BookOrbitDownloadActivity::runDownload() {
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  cancelDownload = false;
  goHomeAfterCancel = false;
  lastRenderedPercent = -1;
  downloadProgress = 0;
  downloadTotal = 0;

  bookorbit::SessionCallbacks callbacks{this, &BookOrbitDownloadActivity::onPhase,
                                        &BookOrbitDownloadActivity::shouldCancel};
  bookorbit::Session session(client, files, renderer, account, /*lendFramebuffer=*/true, callbacks);
  bookorbit::ClientError error = session.connect();
  if (error != bookorbit::ClientError::OK) {
    fail(error == bookorbit::ClientError::FILE ? tr(STR_BOOKORBIT_ERR_STORAGE) : bookOrbitErrorText(error));
    return;
  }
  bookorbit::ManagedFile record;
  std::string finalPath;
  error = session.downloadBook(bookId, record, finalPath);
  if (error == bookorbit::ClientError::ABORTED) {
    LOG_INF(TAG, "download cancelled");
    if (goHomeAfterCancel) {
      onGoHome();
      return;
    }
    leaveToLibrary();
    return;
  }
  if (error != bookorbit::ClientError::OK) {
    fail(error == bookorbit::ClientError::NOT_FOUND ? tr(STR_BOOKORBIT_NO_EPUB) : bookOrbitErrorText(error));
    return;
  }
  setState(OPENING);
  if (BOOKORBIT_STORE.getSyncProgress()) {
    RenderLock lock(*this);  // locator mapping borrows the framebuffer
    session.checkIn(record, finalPath);
  }
  openBook(finalPath);
}

void BookOrbitDownloadActivity::openBook(const std::string& path) {
  client.end();
  if (!Storage.exists(path.c_str())) {
    fail(tr(STR_BOOKORBIT_ERR_STORAGE));
    return;
  }
  if (BOOKORBIT_AGENT.wifiHeld()) {
    // The agent keeps the link; open the reader without the defrag reboot.
    onSelectBook(path);
    return;
  }
  // One-shot continuation: the reboot target selects the reader and the
  // saved path names the book; both are consumed by setup().
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  silentRestartToReader();
  // Only reached when a deep sleep is in progress and the restart was skipped.
  onSelectBook(path);
}

void BookOrbitDownloadActivity::leaveToLibrary() {
  client.end();
  if (BOOKORBIT_AGENT.wifiHeld()) {
    activityManager.goToLibrary();
    return;
  }
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  silentRestartToLibrary();
  onGoHome();
}

void BookOrbitDownloadActivity::screenFn(UiScreen& screen, void* user) {
  static_cast<BookOrbitDownloadActivity*>(user)->buildScreen(screen);
}

void BookOrbitDownloadActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<BookOrbitDownloadActivity*>(user);
  if (self->state != DOWNLOADING) return;
  self->app.clearTapFlash();
  self->cancelDownload = true;
}

void BookOrbitDownloadActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.takeBottom(static_cast<int16_t>(metrics.buttonHintsHeight));
  screen.spacer(static_cast<int16_t>(metrics.topPadding));
  fui::HeaderProps header;
  header.title = tr(STR_BOOKORBIT);
  header.borderEdges = fui::EdgeBottom;
  screen.header(header);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  fui::TextStyle titleStyle = centered;
  titleStyle.maxLines = 2;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const int16_t barH = 16;
  const int16_t btnH = theme.rowHeight;

  const char* status = nullptr;
  switch (state) {
    case PREPARING:
      status = tr(STR_BOOKORBIT_PREPARING);
      break;
    case DOWNLOADING:
      status = tr(STR_DOWNLOADING);
      break;
    case VERIFYING:
      status = tr(STR_BOOKORBIT_VERIFYING);
      break;
    case OPENING:
      status = tr(STR_BOOKORBIT_OPENING);
      break;
    case FAILED:
      status = tr(STR_ERROR_MSG);
      break;
    default:
      status = tr(STR_CHECKING_WIFI);
      break;
  }

  const bool showBar = state == DOWNLOADING;
  const bool showCancel = state == DOWNLOADING;
  const int16_t blockH = static_cast<int16_t>(lh * 3 + (showBar ? barH + gap : 0) + (showCancel ? btnH + gap : 0) +
                                              gap * 2 + (state == FAILED ? lh + gap : 0));
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), status, centered);
  // Draining the queue: "3 of 7" ahead of the current title.
  std::string shown = title;
  if (bookId == QUEUE && queueTotal > 0) {
    char progress[32];
    snprintf(progress, sizeof(progress), tr(STR_BOOKORBIT_QUEUE_PROGRESS), static_cast<unsigned>(queueDone + 1),
             static_cast<unsigned>(queueTotal));
    shown = std::string(progress) + (title.empty() ? "" : " · " + title);
  }
  screen.target().text(screen.takeTop(static_cast<int16_t>(lh * 2), gap), shown.c_str(), titleStyle);
  if (state == FAILED) {
    screen.target().text(screen.takeTop(lh, gap), errorMessage.c_str(), centered);
    if (mappedInput.hasTouch()) screen.target().text(screen.takeTop(lh), tr(STR_TAP_TO_RETRY), centered);
    return;
  }
  if (showBar) {
    const fui::Rect bar = screen.takeTop(barH, gap).inset(fui::Insets{0, 50, 0, 50});
    if (downloadTotal > 0) {
      fui::ProgressBarProps progress;
      progress.value = static_cast<int32_t>(downloadProgress);
      progress.max = static_cast<int32_t>(downloadTotal);
      progress.border = fui::Paint::solid(fui::Color::Black);
      progress.borderWidth = 1;
      fui::progressBar(screen.frame(), bar, progress);
    }
  }
  if (showCancel) {
    const fui::Rect btnArea = screen.takeTop(btnH);
    const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
    fui::ButtonProps cancel;
    cancel.label = tr(STR_CANCEL);
    cancel.action = ACTION_CANCEL;
    screen.button(cancel,
                  fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
  }
}

void BookOrbitDownloadActivity::render(RenderLock&&) {
  renderer.clearScreen();
  MappedInputManager::Labels labels;
  switch (state) {
    case DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case FAILED:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderUi();
  renderer.displayBuffer();
}

void BookOrbitDownloadActivity::loop() {
  if (state == WIFI_SELECTION) return;
  if (state == FAILED) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        setState(PREPARING);
        runDownload();
      } else {
        state = WIFI_SELECTION;
        requestUpdate();
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                               [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      leaveToLibrary();
    }
    return;
  }
  // PREPARING / VERIFYING / OPENING run synchronously inside runDownload();
  // DOWNLOADING pumps its own input from the session's cancel poll.
}
