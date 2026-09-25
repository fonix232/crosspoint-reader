#include "EpubReaderHighlightsActivity.h"

#include <BookOrbitCredentialStore.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "bookorbit/BookOrbitAgent.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;
EpubReaderHighlightsActivity::EpubReaderHighlightsActivity(GfxRenderer& r, MappedInputManager& i,
                                                           const std::string& path)
    : UiListActivity("Highlights", r, i, true),
      store(
          bookorbit::annotationDirectory(BOOKORBIT_STORE.getAccountKey(), path, bookorbit::AnnotationKind::Highlight)) {
  keys.reserve(20);
  titles.reserve(20);
  rows.reserve(22);
}
void EpubReaderHighlightsActivity::onEnter() {
  BOOKORBIT_AGENT.pause();
  reload();
  UiListActivity::onEnter();
}
void EpubReaderHighlightsActivity::onExit() {
  BOOKORBIT_AGENT.resume();
  UiListActivity::onExit();
}
const char* EpubReaderHighlightsActivity::headerTitle() const { return tr(STR_HIGHLIGHTS); }
void EpubReaderHighlightsActivity::reload() {
  keys.clear();
  titles.clear();
  rows.clear();
  hasNext = false;
  failed = !store.open();
  unsigned live = 0;
  bookorbit::Annotation entry;
  for (unsigned i = 0; !failed && i < store.count(); ++i) {
    if (!store.read(i, entry)) {
      failed = true;
      break;
    }
    if (entry.deleted) continue;
    if (live++ < page * 20) continue;
    if (keys.size() == 20) {
      hasNext = true;
      break;
    }
    keys.push_back(entry.key);
    titles.push_back(entry.text.substr(0, 192));
  }
  if (keys.empty() && page > 0) {
    --page;
    reload();
    return;
  }
  for (const auto& title : titles) {
    fui::ListItem row;
    row.label = title.c_str();
    row.actionValue = rows.size();
    rows.push_back(row);
  }
  if (page > 0) {
    fui::ListItem row;
    row.label = tr(STR_BOOKORBIT_PREVIOUS_PAGE);
    row.actionValue = rows.size();
    rows.push_back(row);
  }
  if (hasNext) {
    fui::ListItem row;
    row.label = tr(STR_NEXT_PAGE);
    row.actionValue = rows.size();
    rows.push_back(row);
  }
  nav.reset();
}
void EpubReaderHighlightsActivity::activateIndex(int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (index >= static_cast<int>(keys.size())) {
    if (page > 0 && index == static_cast<int>(keys.size()))
      --page;
    else
      ++page;
    reload();
    requestUpdate();
    return;
  }
  bookorbit::Annotation entry;
  if (!store.find(keys[index], entry) || !entry.mapped) return;
  ProgressChangeResult result;
  result.spineIndex = entry.spine;
  result.hasSavedProgress = true;
  result.hasVisibleTextOffset = true;
  result.visibleTextOffset = entry.offset;
  result.xpath = entry.pos;
  result.percentage = entry.percentage;
  setResult(std::move(result));
  finish();
}
void EpubReaderHighlightsActivity::onRowLongPress(int index) {
  if (index < 0 || index >= static_cast<int>(keys.size())) return;
  const auto key = keys[index];
  const StrId options[] = {StrId::STR_CANCEL, StrId::STR_DELETE};
  popup.show(StrId::STR_HIGHLIGHTS, options, 2, 0, [this, key](int choice) {
    if (choice == 1) {
      failed = !store.eraseLocal(key);
      if (!failed) reload();
    }
    requestUpdate();
  });
  requestUpdate();
}
bool EpubReaderHighlightsActivity::handleCustomInput() {
  return popup.handleInput(mappedInput, [this] { requestUpdate(); });
}
bool EpubReaderHighlightsActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 700)) {
    onRowLongPress(nav.selected);
    return true;
  }
  return UiListActivity::handleButtons();
}
void EpubReaderHighlightsActivity::buildScreen(UiScreen& screen) {
  const auto& m = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(m.topPadding + m.headerHeight), 0,
                                                static_cast<int16_t>(m.buttonHintsHeight), 0});
  if (failed || rows.empty()) {
    screen.centeredText(failed ? tr(STR_BOOKORBIT_ERR_STORAGE) : tr(STR_NO_HIGHLIGHTS));
    return;
  }
  fui::ListProps props;
  props.items = rows.data();
  props.count = rows.size();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  screen.list(props);
}
void EpubReaderHighlightsActivity::render(RenderLock&& lock) {
  UiListActivity::render(std::move(lock));
  popup.processRender(renderer, mappedInput);
}
