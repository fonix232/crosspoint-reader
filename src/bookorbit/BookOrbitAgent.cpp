#include "BookOrbitAgent.h"

#include <BookOrbitCatalogCache.h>
#include <BookOrbitCredentialStore.h>
#include <BookOrbitDownloadQueue.h>
#include <BookOrbitFileStore.h>
#include <BookOrbitPaths.h>
#include <BookOrbitReadingTracker.h>
#include <BookOrbitSyncSchedule.h>
#include <BookOrbitSyncState.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"

namespace {
constexpr char TAG[] = "BOAGENT";
#if defined(BOARD_HAS_PSRAM)
constexpr bool SUPPORTED = true;
#else
constexpr bool SUPPORTED = false;
#endif

struct ScopedLock {
  explicit ScopedLock(SemaphoreHandle_t m) : m(m) { xSemaphoreTake(m, portMAX_DELAY); }
  ~ScopedLock() { xSemaphoreGive(m); }
  SemaphoreHandle_t m;
};
}  // namespace

BookOrbitAgent& BookOrbitAgent::instance() {
  static BookOrbitAgent agent;
  return agent;
}

bool BookOrbitAgent::supported() { return SUPPORTED; }

bool BookOrbitAgent::available() const { return SUPPORTED && task != nullptr && BOOKORBIT_STORE.isConfigured(); }

void BookOrbitAgent::begin(GfxRenderer& gfx) {
  if (!SUPPORTED || task) return;
  renderer = &gfx;
  mutex = xSemaphoreCreateMutex();
  queue = xQueueCreate(QUEUE_LENGTH, sizeof(Command));
  if (!mutex || !queue) {
    LOG_ERR(TAG, "cannot create queue");
    return;
  }
  bookorbit::SyncState state;
  bookorbit::loadSyncState(BOOKORBIT_STORE.getAccountKey(), state);
  progressAttemptAt.store(state.progressAttemptAt);
  progressAttemptOk.store(state.progressAttemptOk);
  // Internal RAM only (task stacks cannot live in PSRAM); 12 KiB covers the
  // TLS client's 2 KiB read buffers plus JSON and chapter streaming frames.
  // Pinned to the Arduino core: the task watchdog guards IDLE0 only, and the
  // session's CPU-bound stretches (chapter streaming for locators, the
  // catalogue index sort) run for seconds without blocking. Unpinned, they
  // could occupy core 0 and trip a TASK_WDT reset; on core 1 they time-slice
  // with the UI loop like the foreground sync screens always have.
  if (xTaskCreatePinnedToCore(&BookOrbitAgent::taskTrampoline, "BOAgent", TASK_STACK_BYTES, this, 1, &task,
                              ARDUINO_RUNNING_CORE) != pdPASS) {
    LOG_ERR(TAG, "cannot create task");
    task = nullptr;
  }
}

void BookOrbitAgent::taskTrampoline(void* arg) { static_cast<BookOrbitAgent*>(arg)->taskLoop(); }

bool BookOrbitAgent::enqueue(const Job job, const uint32_t bookId) {
  if (!available()) return false;
  const Command cmd{job, bookId};
  const auto queued = job == Job::Download ? xQueueSendToFront(queue, &cmd, 0) : xQueueSend(queue, &cmd, 0);
  if (queued == pdTRUE) return true;
  LOG_ERR(TAG, "queue full; job %d dropped", static_cast<int>(job));
  return false;
}

void BookOrbitAgent::requestRefresh() { enqueue(Job::Refresh); }

// The queue file is the source of truth; `queuedIds` mirrors its Queued
// entries for the per-row checks the Library makes while painting.
void BookOrbitAgent::reloadQueueMirror(const bookorbit::DownloadQueue& queue) {
  queuedIds.clear();
  for (const auto& item : queue.items()) {
    if (item.state == bookorbit::QueueState::Queued) queuedIds.push_back(item.bookId);
  }
}

bool BookOrbitAgent::requestDownload(const uint32_t bookId) {
  if (!available() || bookId == 0 || activeDownload.load() == bookId) return false;
  bool added;
  {
    ScopedLock lock(mutex);
    bookorbit::DownloadQueue queue;
    queue.load(BOOKORBIT_STORE.getAccountKey());
    added = queue.add(bookId) && queue.save();
    reloadQueueMirror(queue);
  }
  if (!added) {
    LOG_ERR(TAG, "download queue full; %lu dropped", static_cast<unsigned long>(bookId));
    return false;
  }
  gen++;
  if (!downloadJobPending.exchange(true) && !enqueue(Job::Download)) downloadJobPending.store(false);
  return true;
}

void BookOrbitAgent::cancelDownload(const uint32_t bookId) {
  if (!mutex) return;
  {
    ScopedLock lock(mutex);
    bookorbit::DownloadQueue queue;
    queue.load(BOOKORBIT_STORE.getAccountKey());
    if (queue.remove(bookId)) queue.save();
    reloadQueueMirror(queue);
  }
  // A running transfer of this book is abandoned; the session polls the flag.
  if (activeDownload.load() == bookId) cancelRequested.store(true);
  gen++;
}

void BookOrbitAgent::resumeQueue() {
  if (!available()) return;
  bool pending;
  {
    ScopedLock lock(mutex);
    bookorbit::DownloadQueue queue;
    queue.load(BOOKORBIT_STORE.getAccountKey());
    reloadQueueMirror(queue);
    pending = !queuedIds.empty();
  }
  if (pending && !downloadJobPending.exchange(true) && !enqueue(Job::Download)) downloadJobPending.store(false);
}

void BookOrbitAgent::requestProgressSync() {
  if (!progressJobPending.exchange(true) && !enqueue(Job::Progress)) progressJobPending.store(false);
}

void BookOrbitAgent::requestInsights(const uint32_t monthCode, const bool syncReading) {
  enqueue(syncReading ? Job::InsightsRefresh : Job::Insights, monthCode);
}

void BookOrbitAgent::disconnectNow() {
  holdLink.store(false);
  enqueue(Job::Disconnect);
}

bool BookOrbitAgent::isQueued(const uint32_t bookId) const {
  if (!mutex) return false;
  ScopedLock lock(mutex);
  return std::find(queuedIds.begin(), queuedIds.end(), bookId) != queuedIds.end();
}

size_t BookOrbitAgent::queuedCount() const {
  if (!mutex) return 0;
  ScopedLock lock(mutex);
  return queuedIds.size();
}

bool BookOrbitAgent::lockIdle() {
  if (!mutex) return true;
  if (xSemaphoreTake(mutex, 0) != pdTRUE) return false;
  if (busy() || wifiOwned.load() || pausedFlag.load() || (queue && uxQueueMessagesWaiting(queue))) {
    xSemaphoreGive(mutex);
    return false;
  }
  return true;
}

void BookOrbitAgent::unlockIdle() {
  if (mutex) xSemaphoreGive(mutex);
}

void BookOrbitAgent::pause() {
  // Serialize pause with job admission: a dequeued command must either become
  // busy before this lock, or observe paused before touching storage/the radio.
  if (mutex) {
    ScopedLock lock(mutex);
    pausedFlag.store(true);
    cancelRequested.store(true);
  } else {
    pausedFlag.store(true);
    cancelRequested.store(true);
  }
  // Let a running job notice the cancel before the foreground screen takes
  // the radio; a stalled transfer times out on its own.
  while (busy()) vTaskDelay(pdMS_TO_TICKS(100));
}

void BookOrbitAgent::resume() {
  // A foreground sync can finish without rebooting when the link is held.
  if (task && !busy()) {
    bookorbit::SyncState state;
    bookorbit::loadSyncState(BOOKORBIT_STORE.getAccountKey(), state);
    if (state.progressAttemptAt != progressAttemptAt.load() || state.progressAttemptOk != progressAttemptOk.load()) {
      progressAttemptAt.store(state.progressAttemptAt);
      progressAttemptOk.store(state.progressAttemptOk);
      progressAttemptedThisBoot.store(false);
    }
  }
  cancelRequested.store(false);
  pausedFlag.store(false);
  resumeQueue();
}

uint32_t BookOrbitAgent::idleSecondsLeft() const {
  if (!wifiOwned.load() || !holdLink.load() || !SETTINGS.keepWifiOn) return 0;
  const uint32_t idleMs = static_cast<uint32_t>(SETTINGS.wifiIdleMinutes()) * 60000U;
  const uint32_t elapsed = static_cast<uint32_t>(millis() - lastActivityMs.load());
  return elapsed >= idleMs ? 0 : static_cast<uint32_t>((idleMs - elapsed) / 1000);
}

void BookOrbitAgent::touchActivity() { lastActivityMs.store(millis()); }

// --- reader integration ---------------------------------------------------------

void BookOrbitAgent::noteReaderBook(const std::string& path) {
  if (!mutex) return;
  ScopedLock lock(mutex);
  readerPath = path;
  snapshot.valid = false;
  snapshotFresh.store(false);
}

void BookOrbitAgent::offerReaderSnapshot(const bookorbit::ReaderSnapshot& offered) {
  if (!mutex) return;
  ScopedLock lock(mutex);
  snapshot = offered;
  snapshotFresh.store(true);
  locatorWanted.store(false);
}

// Asks the reader task for the open book's locator and waits for it. Returns
// false when no book is open or the reader did not answer in time.
bool BookOrbitAgent::readerSnapshot(bookorbit::ReaderSnapshot& out) {
  {
    ScopedLock lock(mutex);
    if (readerPath.empty()) {
      out.valid = false;
      return false;
    }
    out.path = readerPath;
    snapshotFresh.store(false);
  }
  locatorWanted.store(true);
  for (unsigned long waited = 0; waited < LOCATOR_WAIT_MS && !snapshotFresh.load(); waited += 100) {
    if (cancelRequested.load()) break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  locatorWanted.store(false);
  ScopedLock lock(mutex);
  if (!snapshotFresh.load() || !snapshot.valid) {
    out.valid = false;
    return false;
  }
  out = snapshot;
  return true;
}

// --- main-loop tick ----------------------------------------------------------------

void BookOrbitAgent::loop() {
  if (!available() || pausedFlag.load()) return;
  // Once per boot, a short while after start-up: a missing or stale catalogue
  // (older than CATALOG_STALE_S) is refreshed in the background, so the
  // Library is populated and current without a manual sync.
  if (!bootRefreshChecked && millis() > BOOT_REFRESH_DELAY_MS) {
    bootRefreshChecked = true;
    const std::string account = BOOKORBIT_STORE.getAccountKey();
    bookorbit::SyncState state;
    bookorbit::loadSyncState(account, state);
    const uint32_t now = bookorbit::nowIfClockValid();
    bookorbit::CatalogCache catalog;
    const bool missing = !catalog.open(account);
    catalog.close();
    const bool stale = now != 0 && (state.lastSuccessAt == 0 || now - state.lastSuccessAt > CATALOG_STALE_S);
    if (missing || stale) {
      LOG_INF(TAG, "catalogue %s; refreshing in the background", missing ? "missing" : "stale");
      requestRefresh();
    }
    // Downloads queued before a reboot or sleep pick up where they stopped.
    resumeQueue();
  }
  const uint8_t minutes = BOOKORBIT_STORE.getProgressSyncMinutes();
  if (minutes == 0 || !BOOKORBIT_STORE.getSyncProgress() || busy() || progressJobPending.load() ||
      millis() < BOOT_REFRESH_DELAY_MS) {
    return;
  }
  uint32_t now = bookorbit::nowIfClockValid();
  time_t rtc = 0;
  if (now == 0 && halClock.isAvailable() && halClock.utcNow(rtc) && rtc >= 1704067200) {
    now = static_cast<uint32_t>(rtc);
  }
  if (!bookorbit::progressSyncDue(minutes, now, progressAttemptAt.load(), progressAttemptOk.load(),
                                  progressAttemptedThisBoot.load(),
                                  static_cast<uint32_t>(millis() - progressAttemptMs.load()))) {
    return;
  }
  LOG_INF(TAG, "periodic progress sync due (%u minute interval)", minutes);
  requestProgressSync();
}

void BookOrbitAgent::prepareForSleep() {
  BOOKORBIT_READING.pause();
  if (!task) return;
  cancelRequested.store(true);
  holdLink.store(false);
  for (int i = 0; i < 50 && busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
  // main.cpp tears the radio down itself; only the ownership flag matters.
  wifiOwned.store(false);
}

// --- worker task ---------------------------------------------------------------------

void BookOrbitAgent::taskLoop() {
  Command cmd;
  for (;;) {
    if (xQueueReceive(queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
      runJob(cmd);
      continue;
    }
    // Idle housekeeping: drop a held link once the inactivity window passed.
    if (wifiOwned.load() && !busy()) {
      const bool expired = idleSecondsLeft() == 0;
      if (!holdLink.load() || expired) {
        if (expired && holdLink.load()) LOG_INF(TAG, "Wi-Fi idle timeout; disconnecting");
        holdLink.store(false);
        wifiOff();
      }
    }
  }
}

bool BookOrbitAgent::ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiOwned.load() && pausedFlag.load()) return false;  // a foreground screen's link
    return true;
  }
  // The picker screen loads the store on entry; headless connects must too.
  if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
  if (WIFI_STORE.getCredentialCount() == 0) {
    LOG_ERR(TAG, "no saved Wi-Fi networks");
    return false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);  // modem sleep while idle; transfers lift it themselves
  auto tryNetwork = [&](const WifiCredential& cred) {
    LOG_INF(TAG, "connecting to %s", cred.ssid.c_str());
    WiFi.begin(cred.ssid.c_str(), cred.password.c_str());
    for (unsigned long waited = 0; waited < CONNECT_TIMEOUT_MS; waited += 200) {
      if (WiFi.status() == WL_CONNECTED) return true;
      if (cancelRequested.load()) return false;
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    WiFi.disconnect(false);
    return false;
  };
  bool connected = false;
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (!last.empty()) {
    if (const auto cred = WIFI_STORE.findCredential(last)) connected = tryNetwork(*cred);
  }
  for (size_t i = 0; !connected && i < WIFI_STORE.getCredentialCount(); i++) {
    if (cancelRequested.load()) break;
    const auto cred = WIFI_STORE.getCredentialAt(i);
    if (!cred || cred->ssid == last) continue;
    connected = tryNetwork(*cred);
  }
  if (!connected) {
    WiFi.mode(WIFI_OFF);
    LOG_ERR(TAG, "no saved network reachable");
    return false;
  }
  wifiOwned.store(true);
  touchActivity();
  LOG_INF(TAG, "connected, ip %s", WiFi.localIP().toString().c_str());
  return true;
}

void BookOrbitAgent::wifiOff() {
  if (!wifiOwned.load()) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiOwned.store(false);
  LOG_INF(TAG, "Wi-Fi off");
}

void BookOrbitAgent::onPhase(void* ctx, const bookorbit::SessionPhase phase, const uint32_t a, const uint32_t b) {
  auto* self = static_cast<BookOrbitAgent*>(ctx);
  self->currentPhase.store(phase);
  self->phaseValueA.store(a);
  self->phaseValueB.store(b);
  self->touchActivity();
}

bool BookOrbitAgent::shouldCancel(void* ctx) {
  auto* self = static_cast<BookOrbitAgent*>(ctx);
  const Job job = self->currentJob.load();
  // User downloads yield only background maintenance, at the session's safe boundaries.
  return self->cancelRequested.load() ||
         (self->downloadJobPending.load() && (job == Job::Progress || job == Job::Refresh));
}

// The Library holds the live catalogue open; wait (bounded) for it to let go
// before the file is replaced underneath it.
void BookOrbitAgent::beforeCatalogInstall(void* ctx) {
  auto* self = static_cast<BookOrbitAgent*>(ctx);
  self->catalogSwap.store(true);
  for (int i = 0; i < 30 && self->libraryHoldsCatalog.load(); i++) vTaskDelay(pdMS_TO_TICKS(100));
}

bool BookOrbitAgent::admitJob(const Command& cmd) {
  ScopedLock lock(mutex);
  if (cmd.job == Job::Download) downloadJobPending.store(false);
  if (pausedFlag.load()) {
    if (cmd.job == Job::Progress) progressJobPending.store(false);
    return false;
  }
  currentJob.store(cmd.job);
  cancelRequested.store(false);
  return true;
}

void BookOrbitAgent::runJob(const Command& cmd) {
  if (!admitJob(cmd)) return;
  if (cmd.job == Job::Disconnect) {
    wifiOff();
    currentJob.store(Job::None);
    return;
  }
  currentPhase.store(bookorbit::SessionPhase::Idle);
  lastFailed.store(false);
  const std::string account = BOOKORBIT_STORE.getAccountKey();
  bookorbit::ClientError error = bookorbit::ClientError::OK;
  bool progressAttempted =
      cmd.job == Job::Progress || (cmd.job == Job::InsightsRefresh && BOOKORBIT_STORE.getSyncProgress());
  bool progressOk = false;

  if (!ensureWifi()) {
    error = bookorbit::ClientError::NETWORK;
  } else {
    bookorbit::Client client;
    bookorbit::FileStore files;
    bookorbit::SessionCallbacks callbacks{this, &BookOrbitAgent::onPhase, &BookOrbitAgent::shouldCancel};
    bookorbit::Session session(client, files, *renderer, account, /*lendFramebuffer=*/false, callbacks);
    error = session.connect();
    if (error == bookorbit::ClientError::OK) {
      switch (cmd.job) {
        case Job::Refresh: {
          bookorbit::SyncState state;
          bookorbit::loadSyncState(account, state);
          const uint32_t startedAt = bookorbit::nowIfClockValid();
          bookorbit::Session::CatalogResult result;
          error = session.refreshCatalog(startedAt, result, &BookOrbitAgent::beforeCatalogInstall, this);
          catalogSwap.store(false);
          gen++;
          if (error == bookorbit::ClientError::OK) {
            session.refreshCovers();
            gen++;
            if (BOOKORBIT_STORE.getSyncProgress()) {
              bookorbit::ReaderSnapshot open;
              readerSnapshot(open);
              std::vector<bookorbit::ProgressConflict> none;
              progressAttempted = true;
              const auto stats = session.syncProgress(open, /*askOnConflict=*/false, none);
              progressOk = stats.errors == 0 && !cancelRequested.load();
              gen++;
            }
            if (!cancelRequested.load()) {
              bookorbit::Session::noteInsightsResult(state, session.refreshInsights(), startedAt);
              gen++;
            }
          }
          state.lastAttemptAt = startedAt;
          state.lastOk = error == bookorbit::ClientError::OK;
          state.lastErrorCode = static_cast<uint8_t>(error);
          if (state.lastOk) {
            state.lastSuccessAt = startedAt;
            state.serverTotal = result.serverTotal;
            state.cachedBooks = result.cached;
          }
          bookorbit::saveSyncState(account, state);
          break;
        }
        case Job::Download: {
          // Drains the queue file front to back; a failure stays in the file
          // (marked) for the queue screen, a success leaves it.
          for (;;) {
            uint32_t bookId = 0;
            {
              ScopedLock lock(mutex);
              bookorbit::DownloadQueue queue;
              queue.load(account);
              reloadQueueMirror(queue);
              if (!queue.nextQueued(bookId)) break;
            }
            if (cancelRequested.load() || pausedFlag.load()) break;
            activeDownload.store(bookId);
            bookorbit::ManagedFile record;
            std::string path;
            const auto result = session.downloadBook(bookId, record, path);
            if (result == bookorbit::ClientError::OK && BOOKORBIT_STORE.getSyncProgress())
              session.checkIn(record, path);
            activeDownload.store(0);
            {
              ScopedLock lock(mutex);
              bookorbit::DownloadQueue queue;
              queue.load(account);
              if (result == bookorbit::ClientError::OK) {
                queue.remove(bookId);
              } else if (result != bookorbit::ClientError::ABORTED) {
                queue.markFailed(bookId, static_cast<uint8_t>(result));
              }
              queue.save();
              reloadQueueMirror(queue);
            }
            gen++;
            if (result == bookorbit::ClientError::ABORTED) break;
            if (result == bookorbit::ClientError::AUTH || result == bookorbit::ClientError::FORBIDDEN ||
                result == bookorbit::ClientError::NETWORK || result == bookorbit::ClientError::TLS_FAILED ||
                result == bookorbit::ClientError::LOW_MEMORY) {
              error = result;  // the rest of the queue waits for the next attempt
              break;
            }
          }
          break;
        }
        case Job::Insights:
        case Job::InsightsRefresh: {
          if (progressAttempted) {
            bookorbit::ReaderSnapshot open;
            readerSnapshot(open);
            std::vector<bookorbit::ProgressConflict> none;
            const auto stats = session.syncProgress(open, /*askOnConflict=*/false, none);
            progressOk = stats.errors == 0 && !cancelRequested.load();
            LOG_INF(TAG, "insights reading sync: %u uploaded, %u applied, %u conflicts, %u errors", stats.uploaded,
                    stats.applied, stats.conflicts, stats.errors);
            gen++;
          }
          if (cancelRequested.load()) {
            error = bookorbit::ClientError::ABORTED;
            break;
          }
          bookorbit::SyncState state;
          bookorbit::loadSyncState(account, state);
          const uint32_t code = cmd.bookId;
          error = session.refreshInsights(static_cast<uint16_t>(code / 100), static_cast<uint8_t>(code % 100));
          bookorbit::Session::noteInsightsResult(state, error, bookorbit::nowIfClockValid());
          bookorbit::saveSyncState(account, state);
          if (progressAttempted && !progressOk && error == bookorbit::ClientError::OK) {
            error = bookorbit::ClientError::SERVER;
          }
          gen++;
          break;
        }
        case Job::Progress: {
          bookorbit::ReaderSnapshot open;
          readerSnapshot(open);
          std::vector<bookorbit::ProgressConflict> none;
          const auto stats = session.syncProgress(open, /*askOnConflict=*/false, none);
          progressOk = stats.errors == 0 && !cancelRequested.load();
          if (!progressOk) {
            error = cancelRequested.load() ? bookorbit::ClientError::ABORTED : bookorbit::ClientError::SERVER;
          }
          LOG_INF(TAG, "progress: %u uploaded, %u applied, %u conflicts, %u errors", stats.uploaded, stats.applied,
                  stats.conflicts, stats.errors);
          gen++;
          break;
        }
        default:
          break;
      }
    }
    client.end();
  }

  if (progressAttempted) {
    bookorbit::SyncState state;
    bookorbit::loadSyncState(account, state);
    state.progressAttemptAt = bookorbit::nowIfClockValid();
    time_t rtc = 0;
    if (state.progressAttemptAt == 0 && halClock.isAvailable() && halClock.utcNow(rtc) && rtc >= 1704067200) {
      state.progressAttemptAt = static_cast<uint32_t>(rtc);
    }
    state.progressAttemptOk = progressOk;
    if (!bookorbit::saveSyncState(account, state)) LOG_ERR(TAG, "cannot save progress schedule");
    progressAttemptAt.store(state.progressAttemptAt);
    progressAttemptOk.store(progressOk);
    progressAttemptMs.store(millis());
    progressAttemptedThisBoot.store(true);
  }
  if (cmd.job == Job::Progress) progressJobPending.store(false);
  if (error != bookorbit::ClientError::OK) {
    LOG_ERR(TAG, "job %d failed: %s", static_cast<int>(cmd.job), bookorbit::clientErrorName(error));
    lastFailed.store(true);
    lastErr.store(error);
  }
  catalogSwap.store(false);
  currentPhase.store(bookorbit::SessionPhase::Idle);
  currentJob.store(Job::None);
  touchActivity();
  // Keep an existing job connection warm only for the configured idle window.
  // The setting never initiates connections or retries a dropped idle link.
  holdLink.store(SETTINGS.keepWifiOn != 0);
  // Without a held link the radio goes down right after the job; a queued
  // follow-up job brings it back up.
  if (!holdLink.load() && uxQueueMessagesWaiting(queue) == 0) wifiOff();
  // Screens poll generation()/busy() from the main task; requestUpdate() is
  // main-task only by contract.
}
