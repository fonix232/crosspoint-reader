#pragma once

#include <BookOrbitClient.h>
#include <BookOrbitFileStore.h>
#include <BookOrbitSession.h>

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

// Resolves a flagged reading-position conflict for one downloaded BookOrbit
// book (a background sync only flags them). Connects, fetches both
// positions, lets the user keep the device's or take BookOrbit's, applies
// the choice, and returns to the Library or Home (rebooting like every Wi-Fi screen
// unless the agent holds the link).
class BookOrbitConflictActivity final : public Activity, private UiAppHost {
 public:
  enum class ReturnTo { Library, Home };
  BookOrbitConflictActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint32_t bookId, std::string title,
                            ReturnTo returnTo = ReturnTo::Library);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == WIFI_SELECTION || state == LOADING || state == RESOLVING; }

 private:
  enum State { WIFI_SELECTION, LOADING, CHOOSE, RESOLVING, DONE, FAILED };
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  static constexpr unsigned long AUTO_RETURN_MS = 1500;

  const ReturnTo returnTo;
  uint32_t bookId;
  std::string title;
  State state = WIFI_SELECTION;
  std::string statusMessage;
  bool wifiActivated = false;
  int selectedOption = 0;
  unsigned long autoReturnAt = 0;

  bookorbit::Client client;
  bookorbit::FileStore files;
  std::string account;
  bookorbit::ProgressConflict conflict;

  void onWifiSelectionComplete(bool success);
  void loadConflict();
  void resolve(bool keepLocal);
  void finish(bool ok, const char* message);
  void leave();
  void restartToOrigin();
  static bool shouldCancel(void* ctx);
  static void screenFn(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildChoiceScreen(UiScreen& screen);
};
