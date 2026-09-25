#pragma once

#include <BookOrbitClient.h>
#include <BookOrbitFileStore.h>
#include <BookOrbitSession.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

// One bounded BookOrbit network session on screen: refresh the catalogue
// cache, fetch a quota of missing cover tiles, and exchange reading progress,
// asking the user about conflicts. Runs on the main loop like the OPDS
// browser (input is pumped between steps so Back can cancel). Reboots on exit
// like every Wi-Fi activity unless the background agent holds the link, and
// lands on the Library or the BookOrbit settings screen.
class BookOrbitSyncActivity final : public Activity, private UiAppHost {
 public:
  enum class ReturnTo { Settings, Library, Insights };

  // `insightsMonthCode` (year * 100 + month) names the calendar month the
  // insights step fetches; 0 means the current one.
  BookOrbitSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ReturnTo returnTo,
                        uint32_t insightsMonthCode = 0);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == RUNNING || state == CONFLICT || state == WIFI_SELECTION; }

 private:
  enum State { WIFI_SELECTION, RUNNING, CONFLICT, DONE, FAILED };
  static constexpr unsigned long AUTO_RETURN_MS = 1500;
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;

  ReturnTo returnTo;
  uint32_t insightsMonthCode;
  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string detailMessage;
  uint32_t completedItems = 0;
  uint32_t totalItems = 0;
  char readingEvents[96] = {};
  bookorbit::SessionPhase displayedPhase = bookorbit::SessionPhase::Idle;
  unsigned long lastProgressPaint = 0;
  bool progressRenderLocked = false;
  bool wifiActivated = false;
  bool cancelRequested = false;
  unsigned long autoReturnAt = 0;

  bookorbit::Client client;
  bookorbit::FileStore files;
  std::string account;
  uint32_t sessionStart = 0;
  bookorbit::Session::CatalogResult catalogResult;
  bookorbit::ProgressStats progressStats;
  bool progressAttempted = false;
  bool insightsAttempted = false;
  bookorbit::ClientError insightsError = bookorbit::ClientError::OK;

  std::vector<bookorbit::ProgressConflict> conflicts;
  std::vector<std::string> conflictTitles;
  size_t conflictIndex = 0;
  int selectedOption = 0;

  void onWifiSelectionComplete(bool success);
  void runSession();
  void resolveConflict(bool keepLocal);
  void finishSession(bool ok, bookorbit::ClientError error);
  void leave();
  void restartToOrigin();
  void setStatus(const char* status, const char* detail = nullptr);
  bool pumpCancel();
  static void onPhase(void* ctx, bookorbit::SessionPhase phase, uint32_t a, uint32_t b);
  static bool shouldCancel(void* ctx);

  static void screenFn(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildConflictScreen(UiScreen& screen);
  void drawScreen();
  void updateProgress(bookorbit::SessionPhase phase, uint32_t completed, uint32_t total);
};
