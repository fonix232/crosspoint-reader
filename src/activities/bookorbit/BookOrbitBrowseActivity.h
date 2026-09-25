#pragma once
#include <BookOrbitClient.h>

#include "activities/UiListActivity.h"

// Online pages stay bounded independently of the offline catalogue's cap.
class BookOrbitBrowseActivity final : public UiListActivity {
 public:
  BookOrbitBrowseActivity(GfxRenderer& renderer, MappedInputManager& input, bool libraryPicker = false);
  void onEnter() override;
  void onExit() override;

 private:
  enum class Mode { Home, Books, Collections, Downloads, RecentlyRead, Libraries };
  struct Entry {
    uint32_t bookId = 0;
    std::string id, title, subtitle;
  };
  int listCount() const override { return static_cast<int>(rows.size()); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  void load();
  void rebuildRows();
  void search();
  bool connect();
  bookorbit::Client client;
  Mode mode = Mode::Home;
  unsigned page = 1;
  bool hasNext = false;
  bool connected = false;
  bool wifiActivated = false;
  bool loading = false;
  bool picker = false;
  bool librariesSaved = false;
  bookorbit::LibrarySelection draftLibraries;
  void applyLibraries();
  std::string query, collection, message;
  std::vector<Entry> entries;
  std::vector<freeink::ui::ListItem> rows;
};
