#pragma once

#include <BookOrbitClient.h>
#include <BookOrbitDownloadQueue.h>
#include <BookOrbitSession.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

// Background BookOrbit worker for boards with PSRAM: owns a headless Wi-Fi
// link (saved networks, no picker), runs sessions on its own task, and turns
// the radio off after a period of inactivity.
//
// Jobs: catalogue/cover refresh, book downloads, and the periodic progress
// exchange. "Keep Wi-Fi On" holds the link until the inactivity timer fires;
// otherwise each job brings Wi-Fi up, runs, and drops it again. The reader
// hands over the open book's locator on request so the worker never touches an
// open book's caches, and a displayed page is never repositioned from here.
//
// Without PSRAM (the C3 boards) `available()` is false and every caller falls
// back to the foreground session activities.
class BookOrbitAgent {
 public:
  enum class Job : uint8_t { None, Refresh, Download, Progress, Insights, Disconnect, InsightsRefresh };

  static BookOrbitAgent& instance();
  // Hardware can host the worker (PSRAM board).
  static bool supported();

  void begin(GfxRenderer& renderer);
  // Main-loop tick: schedules the periodic progress exchange.
  void loop();
  // Supported hardware and a configured account.
  bool available() const;

  // --- requests (any task) ----------------------------------------------------
  void requestRefresh();
  // Adds the book to the account's download queue (file-backed, survives a
  // reboot) and starts draining it; false when the queue is full.
  bool requestDownload(uint32_t bookId);
  // Drops a queued (or failed) entry; abandons the transfer when it is the
  // one in flight.
  void cancelDownload(uint32_t bookId);
  // Starts draining whatever the queue file holds (boot, or after a retry).
  void resumeQueue();
  void requestProgressSync();
  // Reading insights snapshot; `monthCode` (year * 100 + month) also fetches
  // that calendar month, 0 fetches the current one.
  void requestInsights(uint32_t monthCode = 0, bool syncReading = false);
  void disconnectNow();
  // Foreground BookOrbit screens take over the radio while they run.
  void pause();
  void resume();

  // --- state (any task) --------------------------------------------------------
  bool wifiHeld() const { return wifiOwned.load(); }
  bool stayConnectedActive() const { return holdLink.load(); }
  bool busy() const { return currentJob.load() != Job::None; }
  bool paused() const { return pausedFlag.load(); }
  Job job() const { return currentJob.load(); }
  bookorbit::SessionPhase phase() const { return currentPhase.load(); }
  uint32_t phaseA() const { return phaseValueA.load(); }
  uint32_t phaseB() const { return phaseValueB.load(); }
  // Bumped whenever the catalogue, file index, or progress changed on the card.
  uint32_t generation() const { return gen.load(); }
  uint32_t downloadingBookId() const { return activeDownload.load(); }
  bool isQueued(uint32_t bookId) const;
  size_t queuedCount() const;
  bool lastJobFailed() const { return lastFailed.load(); }
  bookorbit::ClientError lastError() const { return lastErr.load(); }
  uint32_t idleSecondsLeft() const;
  // Foreground retries replace the worker's previous outcome so Home does not
  // keep displaying a stale failure after the user resolves it.
  void noteForegroundResult(bookorbit::ClientError error) {
    if (error == bookorbit::ClientError::ABORTED) return;
    lastErr.store(error);
    lastFailed.store(error != bookorbit::ClientError::OK);
    gen++;
  }

  // --- reader integration (main task) -------------------------------------------
  void noteReaderBook(const std::string& path);  // empty on close
  bool wantsReaderLocator() const { return locatorWanted.load(); }
  void offerReaderSnapshot(const bookorbit::ReaderSnapshot& snapshot);

  // --- Library coordination (main task) -----------------------------------------
  // True while the catalogue file is about to be replaced; the Library must
  // release its handle and reopen after the generation bump.
  bool catalogBusy() const { return catalogSwap.load(); }
  void setLibraryHoldsCatalog(bool holds) { libraryHoldsCatalog.store(holds); }

  // Cancels the running job and drops the link before deep sleep.
  void prepareForSleep();

 private:
  struct Command {
    Job job;
    uint32_t bookId;
  };
  static constexpr size_t QUEUE_LENGTH = 8;
  static constexpr uint32_t TASK_STACK_BYTES = 12288;
  static constexpr uint32_t CONNECT_TIMEOUT_MS = 12000;
  static constexpr uint32_t LOCATOR_WAIT_MS = 8000;
  static constexpr uint32_t BOOT_REFRESH_DELAY_MS = 15000;
  static constexpr uint32_t CATALOG_STALE_S = 12 * 3600;

  BookOrbitAgent() = default;
  static void taskTrampoline(void* arg);
  void taskLoop();
  bool enqueue(Job job, uint32_t bookId = 0);
  bool admitJob(const Command& cmd);
  void runJob(const Command& cmd);
  bool ensureWifi();
  void wifiOff();
  void touchActivity();
  bool readerSnapshot(bookorbit::ReaderSnapshot& out);
  static void onPhase(void* ctx, bookorbit::SessionPhase phase, uint32_t a, uint32_t b);
  static bool shouldCancel(void* ctx);
  static void beforeCatalogInstall(void* ctx);

  GfxRenderer* renderer = nullptr;
  TaskHandle_t task = nullptr;
  QueueHandle_t queue = nullptr;
  SemaphoreHandle_t mutex = nullptr;  // guards the strings below

  std::atomic<Job> currentJob{Job::None};
  std::atomic<bookorbit::SessionPhase> currentPhase{bookorbit::SessionPhase::Idle};
  std::atomic<uint32_t> phaseValueA{0};
  std::atomic<uint32_t> phaseValueB{0};
  std::atomic<uint32_t> gen{1};
  std::atomic<uint32_t> activeDownload{0};
  std::atomic<bool> wifiOwned{false};
  std::atomic<bool> holdLink{false};
  std::atomic<bool> pausedFlag{false};
  std::atomic<bool> cancelRequested{false};
  std::atomic<bool> lastFailed{false};
  std::atomic<bookorbit::ClientError> lastErr{bookorbit::ClientError::OK};
  std::atomic<bool> locatorWanted{false};
  std::atomic<bool> snapshotFresh{false};
  std::atomic<bool> catalogSwap{false};
  std::atomic<bool> libraryHoldsCatalog{false};
  std::atomic<unsigned long> lastActivityMs{0};
  std::atomic<bool> progressJobPending{false};
  std::atomic<bool> progressAttemptedThisBoot{false};
  std::atomic<uint32_t> progressAttemptMs{0};
  std::atomic<uint32_t> progressAttemptAt{0};
  std::atomic<bool> progressAttemptOk{false};
  bool bootRefreshChecked = false;
  std::vector<uint32_t> queuedIds;  // mirror of the queue file's Queued entries (mutex)
  std::atomic<bool> downloadJobPending{false};
  void reloadQueueMirror(const bookorbit::DownloadQueue& queue);

  std::string readerPath;
  bookorbit::ReaderSnapshot snapshot;
};

#define BOOKORBIT_AGENT BookOrbitAgent::instance()
