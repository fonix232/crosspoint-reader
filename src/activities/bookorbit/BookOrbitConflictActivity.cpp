#include "BookOrbitConflictActivity.h"

#include <BookOrbitCredentialStore.h>
#include <FontCacheManager.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

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
constexpr char TAG[] = "BOCONF";
}  // namespace

BookOrbitConflictActivity::BookOrbitConflictActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const uint32_t bookId, std::string title, const ReturnTo returnTo)
    : Activity("BookOrbitConflict", renderer, mappedInput),
      UiAppHost(renderer),
      returnTo(returnTo),
      bookId(bookId),
      title(std::move(title)) {}

void BookOrbitConflictActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_ROW, &BookOrbitConflictActivity::onRowEvent, this);
  app.setScreen(&BookOrbitConflictActivity::screenFn, this);

  if (!BOOKORBIT_STORE.isConfigured()) {
    finish(false, tr(STR_BOOKORBIT_ERR_NOT_CONFIGURED));
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

void BookOrbitConflictActivity::onExit() {
  Activity::onExit();
  client.end();
  BOOKORBIT_AGENT.resume();
  if (wifiActivated && !BOOKORBIT_AGENT.wifiHeld() && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    restartToOrigin();
  }
}

void BookOrbitConflictActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish(false, tr(STR_WIFI_CONN_FAILED));
    return;
  }
  WiFi.setSleep(false);
  {
    RenderLock lock(*this);
    state = LOADING;
    statusMessage = tr(STR_BOOKORBIT_CONFLICT_LOADING);
  }
  requestUpdateAndWait();
  loadConflict();
}

bool BookOrbitConflictActivity::shouldCancel(void* ctx) {
  auto* self = static_cast<BookOrbitConflictActivity*>(ctx);
  self->mappedInput.update(true);
  return self->mappedInput.wasReleased(MappedInputManager::Button::Back) || self->mappedInput.wasHomeGesture();
}

void BookOrbitConflictActivity::loadConflict() {
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  bookorbit::SessionCallbacks callbacks{this, nullptr, &BookOrbitConflictActivity::shouldCancel};
  bookorbit::Session session(client, files, renderer, account, /*lendFramebuffer=*/true, callbacks);
  bookorbit::ClientError error = session.connect();
  if (error != bookorbit::ClientError::OK) {
    finish(false, error == bookorbit::ClientError::FILE ? tr(STR_BOOKORBIT_ERR_STORAGE) : bookOrbitErrorText(error));
    return;
  }
  bool found = false;
  {
    RenderLock lock(*this);  // locator mapping borrows the framebuffer
    error = session.loadConflict(bookId, conflict, found);
  }
  if (error != bookorbit::ClientError::OK) {
    finish(false, bookOrbitErrorText(error));
    return;
  }
  if (!found) {
    finish(true, tr(STR_BOOKORBIT_CONFLICT_NONE));
    return;
  }
  {
    RenderLock lock(*this);
    state = CHOOSE;
    selectedOption = 0;
  }
  closeRouting();
  requestUpdate();
}

void BookOrbitConflictActivity::resolve(const bool keepLocal) {
  {
    RenderLock lock(*this);
    state = RESOLVING;
    statusMessage = tr(STR_BOOKORBIT_SYNCING_PROGRESS);
  }
  requestUpdateAndWait();
  bookorbit::SessionCallbacks callbacks{this, nullptr, &BookOrbitConflictActivity::shouldCancel};
  bookorbit::Session session(client, files, renderer, account, /*lendFramebuffer=*/true, callbacks);
  bool ok;
  {
    RenderLock lock(*this);
    ok = session.resolveConflict(conflict, keepLocal);
  }
  LOG_INF(TAG, "conflict for %lu: %s (%s)", static_cast<unsigned long>(bookId),
          keepLocal ? "kept device" : "took server", ok ? "ok" : "failed");
  finish(ok, ok ? tr(STR_BOOKORBIT_CONFLICT_RESOLVED) : tr(STR_BOOKORBIT_SYNC_FAILED));
}

void BookOrbitConflictActivity::finish(const bool ok, const char* message) {
  client.end();
  BOOKORBIT_AGENT.noteForegroundResult(ok ? bookorbit::ClientError::OK : bookorbit::ClientError::SERVER);
  if (wifiActivated && !BOOKORBIT_AGENT.wifiHeld() && WiFi.getMode() != WIFI_MODE_NULL) WiFi.disconnect(false);
  {
    RenderLock lock(*this);
    state = ok ? DONE : FAILED;
    statusMessage = message ? message : "";
  }
  if (ok) autoReturnAt = millis() + AUTO_RETURN_MS;
  requestUpdate();
}

void BookOrbitConflictActivity::restartToOrigin() {
  if (returnTo == ReturnTo::Home)
    silentRestart();
  else
    silentRestartToLibrary();
}

void BookOrbitConflictActivity::leave() {
  client.end();
  if (BOOKORBIT_AGENT.wifiHeld()) {
    if (returnTo == ReturnTo::Home)
      onGoHome();
    else
      activityManager.goToLibrary();
    return;
  }
  if (wifiActivated && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  restartToOrigin();
  onGoHome();
}

void BookOrbitConflictActivity::loop() {
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
  if (state != CHOOSE) return;
  const auto route = routeTouch(mappedInput);
  if (route.routed && app.invalidated()) requestUpdate();
  if (route) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedOption = selectedOption == 0 ? 1 : 0;
    closeRouting();
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    resolve(selectedOption == 0);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
    leave();
  }
}

void BookOrbitConflictActivity::screenFn(UiScreen& screen, void* user) {
  static_cast<BookOrbitConflictActivity*>(user)->buildChoiceScreen(screen);
}

void BookOrbitConflictActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitConflictActivity*>(user);
  if (self->state != CHOOSE || event.value < 0 || event.value > 1) return;
  self->app.clearTapFlash();
  self->selectedOption = event.value;
  self->resolve(self->selectedOption == 0);
}

void BookOrbitConflictActivity::buildChoiceScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  auto titleStyle = screen.theme().bodyText;
  titleStyle.bold = true;
  titleStyle.maxLines = 2;
  const int16_t indent = static_cast<int16_t>(screen.theme().listInset + screen.theme().listSidePadding);
  fui::Rect r =
      screen.takeTop(static_cast<int16_t>(screen.target().lineHeight(titleStyle.font) * 2), screen.theme().spaceMd);
  r.x = static_cast<int16_t>(r.x + indent);
  r.width = static_cast<int16_t>(r.width - indent * 2);
  screen.target().text(r, title.empty() ? tr(STR_BOOKORBIT_CONFLICT_TITLE) : title.c_str(), titleStyle);
  auto hintStyle = screen.theme().smallText;
  hintStyle.maxLines = 2;
  fui::Rect h =
      screen.takeTop(static_cast<int16_t>(screen.target().lineHeight(hintStyle.font) * 2), screen.theme().spaceMd);
  h.x = static_cast<int16_t>(h.x + indent);
  h.width = static_cast<int16_t>(h.width - indent * 2);
  screen.target().text(h, tr(STR_BOOKORBIT_CONFLICT_HINT), hintStyle);

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

void BookOrbitConflictActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_BOOKORBIT_CONFLICT_TITLE));
  if (state == CHOOSE) {
    renderUi();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
  const int top = screen.y + screen.height / 2 - 40;
  UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, title.c_str(), true, EpdFontFamily::BOLD);
  if (!statusMessage.empty())
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, statusMessage.c_str());
  const bool finished = state == DONE || state == FAILED;
  const auto labels =
      mappedInput.mapLabels(finished ? tr(STR_BACK) : tr(STR_CANCEL), finished ? tr(STR_DONE) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
