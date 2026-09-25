#pragma once
#include <BookOrbitDataStore.h>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class EpubReaderHighlightsActivity final : public UiListActivity {
 public:
  EpubReaderHighlightsActivity(GfxRenderer& renderer, MappedInputManager& input, const std::string& path);
  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return rows.size(); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void render(RenderLock&& lock) override;
  void reload();
  bookorbit::AnnotationStore store;
  unsigned page = 0;
  bool hasNext = false;
  bool failed = false;
  std::vector<std::string> keys, titles;
  std::vector<freeink::ui::ListItem> rows;
  OptionPopup popup;
};
