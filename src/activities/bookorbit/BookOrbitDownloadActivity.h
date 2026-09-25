#pragma once

#include <BookOrbitClient.h>
#include <BookOrbitFileStore.h>
#include <BookOrbitSession.h>

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

// Foreground download-on-open for one BookOrbit book (the path for boards
// without the background agent, and the fallback when it is unavailable):
// connect, run Session::downloadBook, check the device in, then open the
// reader. Reboots into the reader unless the agent holds the Wi-Fi link.
// With `bookId` QUEUE it drains the account's download queue instead
// (marking failures) and returns to the Library.
class BookOrbitDownloadActivity final : public Activity, private UiAppHost {
 public:
  static constexpr uint32_t QUEUE = 0;

  BookOrbitDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint32_t bookId, std::string title);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state != FAILED; }

 private:
  enum State { WIFI_SELECTION, PREPARING, DOWNLOADING, VERIFYING, OPENING, FAILED };
  static constexpr freeink::ui::ActionId ACTION_CANCEL = 1;
  static constexpr int PROGRESS_STEP_PERCENT = 5;
  static constexpr unsigned long PROGRESS_MIN_UPDATE_MS = 4000;

  uint32_t bookId;
  std::string title;
  State state = WIFI_SELECTION;
  std::string errorMessage;
  uint16_t queueDone = 0;
  uint16_t queueTotal = 0;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool cancelDownload = false;
  bool goHomeAfterCancel = false;
  bool wifiActivated = false;
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;

  bookorbit::Client client;
  bookorbit::FileStore files;
  std::string account;

  void onWifiSelectionComplete(bool success);
  void runDownload();
  void runQueue();
  void fail(const char* message);
  void openBook(const std::string& path);
  void leaveToLibrary();
  void setState(State next);
  static void onPhase(void* ctx, bookorbit::SessionPhase phase, uint32_t a, uint32_t b);
  static bool shouldCancel(void* ctx);

  static void screenFn(UiScreen& screen, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildScreen(UiScreen& screen);
};
