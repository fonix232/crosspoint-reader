#pragma once

#include <string>

#include "activities/Activity.h"

// Connects to Wi-Fi and validates the configured BookOrbit credentials against
// `GET /koreader/users/auth`, then reports the outcome. Reboots on exit like
// every Wi-Fi activity.
class BookOrbitAuthActivity final : public Activity {
 public:
  explicit BookOrbitAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookOrbitAuth", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == AUTHENTICATING; }

 private:
  enum State { WIFI_SELECTION, CONNECTING, AUTHENTICATING, SUCCESS, FAILED };

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string detailMessage;  // server version and device capabilities
  bool wifiActivated = false;

  void onWifiSelectionComplete(bool success);
  void performAuthentication();
};
