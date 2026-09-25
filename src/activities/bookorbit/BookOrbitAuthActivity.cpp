#include "BookOrbitAuthActivity.h"

#include <BookOrbitClient.h>
#include <BookOrbitCredentialStore.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "BookOrbitUi.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "bookorbit/BookOrbitAgent.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BookOrbitAuthActivity::onEnter() {
  Activity::onEnter();
  if (!BOOKORBIT_STORE.isConfigured()) {
    state = FAILED;
    statusMessage = tr(STR_BOOKORBIT_ERR_NOT_CONFIGURED);
    requestUpdate();
    return;
  }
  BOOKORBIT_AGENT.pause();
  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookOrbitAuthActivity::onExit() {
  Activity::onExit();
  BOOKORBIT_AGENT.resume();
  // With the agent holding the link, finish() simply pops back to settings.
  if (wifiActivated && !BOOKORBIT_AGENT.wifiHeld() && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToBookOrbitSettings();
  }
}

void BookOrbitAuthActivity::onWifiSelectionComplete(const bool success) {
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
    state = AUTHENTICATING;
    statusMessage = tr(STR_AUTHENTICATING);
  }
  requestUpdateAndWait();
  performAuthentication();
}

void BookOrbitAuthActivity::performAuthentication() {
  bookorbit::Client client;
  bookorbit::ClientError result = client.begin();
  if (result == bookorbit::ClientError::OK) result = client.authenticate();
  // What this server can do for the device, for the result line.
  std::string detail;
  if (result == bookorbit::ClientError::OK && client.fetchCapabilities() == bookorbit::ClientError::OK) {
    detail = client.serverVersion().empty() ? tr(STR_BOOKORBIT_SERVER_BASE) : client.serverVersion();
    if (client.hasCapability(bookorbit::ServerCapability::DeviceThumbnails))
      detail += std::string(" · ") + tr(STR_BOOKORBIT_CAP_COVERS);
    if (client.hasCapability(bookorbit::ServerCapability::ReadingInsights))
      detail += std::string(" · ") + tr(STR_BOOKORBIT_CAP_INSIGHTS);
  }
  client.end();
  LOG_INF("BO", "auth: %s (http %d)", bookorbit::clientErrorName(result), client.lastHttpCode());
  {
    RenderLock lock(*this);
    if (result == bookorbit::ClientError::OK) {
      state = SUCCESS;
      statusMessage = tr(STR_BOOKORBIT_CONNECTED);
      detailMessage = detail;
    } else {
      state = FAILED;
      statusMessage = bookOrbitErrorText(result);
    }
  }
  requestUpdate();
}

void BookOrbitAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_BOOKORBIT_TEST_CONNECTION));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;
  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    if (!detailMessage.empty()) renderer.drawCenteredText(SMALL_FONT_ID, top + height + 10, detailMessage.c_str());
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_FAILED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, statusMessage.c_str());
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BookOrbitAuthActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
  }
}
