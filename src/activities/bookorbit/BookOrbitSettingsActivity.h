#pragma once

#include <string>

#include "activities/UiListActivity.h"

// BookOrbit account settings: server, credentials, TLS and sync toggles, a
// connection test, a manual library sync, and sign-out. Patterned on
// KOReaderSettingsActivity; the two stores are independent.
class BookOrbitSettingsActivity final : public UiListActivity {
 public:
  explicit BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int MENU_ITEMS = 12;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  // Repaints the Sync now row as the background agent's job state moves.
  bool handleCustomInput() override;

  void editServerUrl();
  void editUsername();
  void editPassword();
  void testConnection();
  void syncNow();
  void openQueue();
  void promptSignOut();
  void cycleSyncInterval();

  uint8_t lastAgentState = 0xFF;
  std::string rowValues_[MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MENU_ITEMS]{};
};
