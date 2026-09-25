#include "BookOrbitBrowseActivity.h"

#include <ArduinoJson.h>
#include <BookOrbitCatalogCache.h>
#include <BookOrbitCredentialStore.h>
#include <BookOrbitDataProtocol.h>
#include <BookOrbitFileStore.h>
#include <BookOrbitPaths.h>
#include <Memory.h>
#include <PersistableStore.h>
#include <WiFi.h>

#include "BookOrbitDownloadActivity.h"
#include "BookOrbitUi.h"
#include "CrossPointState.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "bookorbit/BookOrbitAgent.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;
BookOrbitBrowseActivity::BookOrbitBrowseActivity(GfxRenderer& r, MappedInputManager& i, const bool libraryPicker)
    : UiListActivity("BookOrbitBrowse", r, i) {
  picker = libraryPicker;
  if (picker) mode = Mode::Libraries;
  draftLibraries = BOOKORBIT_STORE.getLibraries();
  entries.reserve(20);
  rows.reserve(25);
}
const char* BookOrbitBrowseActivity::headerTitle() const {
  if (picker) return tr(STR_BOOKORBIT_LIBRARIES);
  if (mode == Mode::RecentlyRead) return tr(STR_BOOKORBIT_RECENTLY_READ);
  return tr(STR_BOOKORBIT_BROWSE);
}
void BookOrbitBrowseActivity::onEnter() {
  BOOKORBIT_AGENT.pause();
  rebuildRows();
  UiListActivity::onEnter();
  if (picker) load();
}
void BookOrbitBrowseActivity::onExit() {
  client.end();
  BOOKORBIT_AGENT.resume();
  if (librariesSaved) BOOKORBIT_AGENT.requestRefresh();
  UiListActivity::onExit();
  if (wifiActivated && !BOOKORBIT_AGENT.wifiHeld() && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    silentRestartToLibrary();
  }
}
bool BookOrbitBrowseActivity::connect() {
  if (WiFi.status() != WL_CONNECTED) {
    auto picker = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
    if (!picker) {
      message = tr(STR_BOOKORBIT_ERR_MEMORY);
      return false;
    }
    wifiActivated = true;
    startActivityForResult(std::move(picker), [this](const ActivityResult& result) {
      if (!result.isCancelled)
        load();
      else {
        message = tr(STR_WIFI_CONN_FAILED);
        rebuildRows();
        requestUpdate();
      }
    });
    return false;
  }
  WiFi.setSleep(false);
  if (connected) return true;
  auto error = client.begin();
  if (error == bookorbit::ClientError::OK) error = client.authenticate();
  connected = error == bookorbit::ClientError::OK;
  if (!connected) message = bookOrbitErrorText(error);
  return connected;
}
void BookOrbitBrowseActivity::load() {
  {
    RenderLock lock(*this);
    loading = true;
    message.clear();
  }
  requestUpdateAndWait();
  if (mode != Mode::Downloads && !connect()) {
    loading = false;
    rebuildRows();
    requestUpdate();
    return;
  }
  std::vector<Entry> incoming;
  incoming.reserve(20);
  bool next = false;
  bookorbit::ClientError error;
  if (mode == Mode::Downloads) {
    bookorbit::FileStore files;
    bookorbit::CatalogCache cache;
    const auto account = BOOKORBIT_STORE.getAccountKey();
    cache.open(account);
    error = files.open(account) ? bookorbit::ClientError::OK : bookorbit::ClientError::FILE;
    unsigned live = 0;
    bookorbit::ManagedFile file;
    for (unsigned i = 0; i < files.count(); ++i) {
      if (!files.read(i, file) || !file.complete()) continue;
      const std::string path = files.pathFor(file);
      if (path.empty() || !Storage.exists(path.c_str())) continue;
      if (live++ < (page - 1) * 20) continue;
      if (incoming.size() == 20) {
        next = true;
        break;
      }
      Entry entry;
      entry.bookId = file.bookId;
      entry.id = path;
      bookorbit::CatalogRecord record;
      const auto ordinal = cache.ordinalForBook(file.bookId);
      if (ordinal != 0xffff && cache.readRecord(ordinal, record)) {
        entry.title = record.titleView();
        entry.subtitle = record.authorView();
      } else {
        JsonDocument title;
        PersistableStoreBase::readDocFromFile(
            (bookorbit::accountDir(account) + "/title-" + std::to_string(file.bookId) + ".json").c_str(), title);
        entry.title = title["title"] | "";
        if (entry.title.empty()) entry.title = path.substr(path.find_last_of('/') + 1);
      }
      incoming.push_back(std::move(entry));
    }
  } else if (mode == Mode::Collections || mode == Mode::Libraries) {
    struct Collections {
      std::vector<Entry>& list;
      unsigned page, seen = 0;
      int depth = 0;
      bool ended = false, overflow = false;
      std::string field;
      Entry current;
      StreamingJsonParser parser;
      Collections(std::vector<Entry>& list, unsigned page)
          : list(list),
            page(page),
            parser(JsonCallbacks{
                this, [](void* c, const char* v, size_t n) { static_cast<Collections*>(c)->field.assign(v, n); },
                [](void* c, const char* v, size_t n) {
                  auto& s = *static_cast<Collections*>(c);
                  if (s.depth != 2) return;
                  std::string* target = s.field == "id"      ? &s.current.id
                                        : s.field == "title" ? &s.current.title
                                                             : nullptr;
                  if (target) {
                    target->assign(v, n);
                    target->resize(bookorbit::decodeJsonUnicodeEscapes(target->data(), target->size()));
                  }
                },
                [](void* c, const char* v, size_t n) {
                  auto& s = *static_cast<Collections*>(c);
                  if (s.depth == 2 && s.field == "id") s.current.id.assign(v, n);
                  if (s.depth == 2 && s.field == "count") s.current.subtitle.assign(v, n);
                },
                nullptr, nullptr,
                [](void* c) {
                  auto& s = *static_cast<Collections*>(c);
                  if (++s.depth == 2) s.current = Entry{};
                },
                [](void* c) {
                  auto& s = *static_cast<Collections*>(c);
                  if (s.depth == 2 && !s.current.id.empty()) {
                    if (s.seen >= (s.page - 1) * 20 && s.list.size() < 20) s.list.push_back(std::move(s.current));
                    ++s.seen;
                  }
                  if (--s.depth == 0) s.ended = true;
                },
                nullptr, nullptr, [](void* c) { static_cast<Collections*>(c)->overflow = true; }}) {}
    };
    auto parser = makeUniqueNoThrow<Collections>(incoming, page);
    if (!parser)
      error = bookorbit::ClientError::LOW_MEMORY;
    else {
      error = client.getStream(
          mode == Mode::Libraries ? "/koreader/plugin/catalog/sections/libraries"
                                  : "/koreader/plugin/catalog/sections/collections",
          [](void* ctx, const char* bytes, size_t size) {
            auto& s = *static_cast<Collections*>(ctx);
            s.parser.feed(bytes, size);
            return !s.parser.hasError() && !s.overflow;
          },
          parser.get());
      if (error == bookorbit::ClientError::OK && (!parser->ended || parser->overflow || parser->parser.hasError()))
        error = bookorbit::ClientError::JSON;
      next = parser->seen > page * 20;
    }
  } else {
    struct PageContext {
      std::vector<Entry>& entries;
      bool recentlyRead;
    } context{incoming, mode == Mode::RecentlyRead};
    auto parser = makeUniqueNoThrow<bookorbit::CatalogPageParser>(
        [](void* ctx, const bookorbit::CatalogRecord& book) {
          auto& context = *static_cast<PageContext*>(ctx);
          // The server sort also returns unread books. Keep only recorded reading activity.
          if (context.recentlyRead && book.lastReadAt == 0) return true;
          auto& list = context.entries;
          if (list.size() >= 20) return false;
          Entry entry;
          entry.bookId = book.bookId;
          entry.title = book.titleView();
          entry.subtitle = book.authorView();
          list.push_back(std::move(entry));
          return true;
        },
        &context);
    if (!parser)
      error = bookorbit::ClientError::LOW_MEMORY;
    else {
      const std::string filters = "&q=" + bookorbit::encodeQuery(query) +
                                  (collection.empty() ? "" : "&collectionId=" + bookorbit::encodeQuery(collection));
      error = client.fetchCatalogPage(
          page, 20, *parser, filters,
          mode == Mode::RecentlyRead ? bookorbit::CatalogSort::RecentlyRead : bookorbit::CatalogSort::Title);
      next = static_cast<uint32_t>(page) * 20 < parser->total();
    }
  }
  {
    RenderLock lock(*this);
    loading = false;
    if (error == bookorbit::ClientError::OK) {
      entries = std::move(incoming);
      hasNext = next;
      message.clear();
    } else {
      entries.clear();
      hasNext = false;
      message = bookOrbitErrorText(error);
    }
    nav.reset();
    rebuildRows();
  }
  requestUpdate();
}
void BookOrbitBrowseActivity::rebuildRows() {
  rows.clear();
  auto add = [&](const char* label, const char* subtitle = nullptr) {
    fui::ListItem row;
    row.label = label;
    row.subtitle = subtitle;
    row.actionValue = rows.size();
    rows.push_back(row);
  };
  if (mode == Mode::Home) {
    add(tr(STR_BOOKORBIT_ALL_BOOKS));
    add(tr(STR_SEARCH));
    add(tr(STR_BOOKORBIT_COLLECTIONS));
    add(tr(STR_BOOKORBIT_ON_DEVICE));
    add(tr(STR_BOOKORBIT_RECENTLY_READ));
  } else {
    if (picker) {
      add(tr(STR_BOOKORBIT_ALL_LIBRARIES), draftLibraries.all ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
      add(tr(STR_BOOKORBIT_APPLY_LIBRARIES));
      for (const auto& entry : entries) {
        const uint32_t id = strtoul(entry.id.c_str(), nullptr, 10);
        add(entry.title.c_str(), draftLibraries.contains(id) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
      }
    } else {
      add(tr(STR_SEARCH));
      for (const auto& entry : entries) add(entry.title.c_str(), entry.subtitle.c_str());
    }
    if (page > 1) add(tr(STR_BOOKORBIT_PREVIOUS_PAGE));
    if (hasNext) add(tr(STR_NEXT_PAGE));
    if (!message.empty()) add(tr(STR_BOOKORBIT_RETRY));
  }
}
void BookOrbitBrowseActivity::search() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query, 96,
                                                           InputType::Text);
  if (!keyboard) return;
  client.disconnect();
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    query = std::get<KeyboardResult>(result.data).text;
    if (mode != Mode::RecentlyRead) mode = Mode::Books;
    page = 1;
    load();
  });
}
void BookOrbitBrowseActivity::activateIndex(int index) {
  if (loading || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (picker) {
    if (index == 0) {
      draftLibraries.all = !draftLibraries.all;
      draftLibraries.count = 0;
      rebuildRows();
      requestUpdate();
      return;
    }
    if (index == 1) {
      applyLibraries();
      return;
    }
    if (index < static_cast<int>(entries.size()) + 2) {
      const auto& idText = entries[index - 2].id;
      char* end = nullptr;
      const unsigned long id = strtoul(idText.c_str(), &end, 10);
      if (!id || *end || id > UINT32_MAX || !draftLibraries.toggle(static_cast<uint32_t>(id)))
        message = tr(STR_BOOKORBIT_LIBRARY_LIMIT);
      rebuildRows();
      requestUpdate();
      return;
    }
    const int after = index - 2 - entries.size();
    if (page > 1 && after == 0)
      --page;
    else if (hasNext && after == (page > 1 ? 1 : 0))
      ++page;
    load();
    return;
  }
  if (mode == Mode::Home) {
    if (index == 1) {
      search();
      return;
    }
    mode = index == 4   ? Mode::RecentlyRead
           : index == 3 ? Mode::Downloads
           : index == 2 ? Mode::Collections
                        : Mode::Books;
    page = 1;
    query.clear();
    collection.clear();
    load();
    return;
  }
  if (index == 0) {
    search();
    return;
  }
  if (index <= static_cast<int>(entries.size())) {
    const auto entry = entries[index - 1];
    if (mode == Mode::Collections) {
      collection = entry.id;
      mode = Mode::Books;
      page = 1;
      load();
      return;
    }
    if (mode == Mode::Downloads) {
      client.end();
      if (wifiActivated && !BOOKORBIT_AGENT.wifiHeld()) {
        APP_STATE.openEpubPath = entry.id;
        APP_STATE.saveToFile();
        WiFi.disconnect(false);
        silentRestartToReader();
      }
      onSelectBook(entry.id);
      return;
    }
    const auto account = BOOKORBIT_STORE.getAccountKey();
    Storage.mkdir(bookorbit::accountDir(account).c_str(), true);
    JsonDocument title;
    title["title"] = entry.title;
    PersistableStoreBase::writeDocToFile(
        (bookorbit::accountDir(account) + "/title-" + std::to_string(entry.bookId) + ".json").c_str(), title);
    client.disconnect();
    auto download = makeUniqueNoThrow<BookOrbitDownloadActivity>(renderer, mappedInput, entry.bookId, entry.title);
    if (!download) return;
    startActivityForResult(std::move(download), [this](const ActivityResult&) {
      BOOKORBIT_AGENT.pause();
      connected = false;
      requestUpdate();
    });
    return;
  }
  const int after = index - 1 - entries.size();
  if (page > 1 && after == 0)
    --page;
  else if (hasNext && after == (page > 1 ? 1 : 0))
    ++page;
  load();
}
void BookOrbitBrowseActivity::applyLibraries() {
  const auto previous = BOOKORBIT_STORE.getLibraries();
  if (previous.key() == draftLibraries.key()) {
    finish();
    return;
  }
  BOOKORBIT_STORE.setLibraries(draftLibraries);
  if (!BOOKORBIT_STORE.saveToFile()) {
    BOOKORBIT_STORE.setLibraries(previous);
    message = tr(STR_BOOKORBIT_ERR_STORAGE);
    rebuildRows();
    requestUpdate();
    return;
  }
  librariesSaved = true;
  finish();
}

void BookOrbitBrowseActivity::onBackButton() {
  if (picker || mode == Mode::Home) {
    finish();
    return;
  }
  mode = Mode::Home;
  page = 1;
  entries.clear();
  message.clear();
  nav.reset();
  rebuildRows();
  requestUpdate();
}
void BookOrbitBrowseActivity::buildScreen(UiScreen& screen) {
  const auto& m = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(m.topPadding + m.headerHeight), 0,
                                                static_cast<int16_t>(m.buttonHintsHeight), 0});
  if (loading) {
    screen.centeredText(tr(STR_LOADING));
    return;
  }
  if (!message.empty())
    screen.centeredText(message.c_str());
  else if (mode == Mode::RecentlyRead && entries.empty())
    screen.centeredText(tr(STR_BOOKORBIT_NO_RECENT_ON_PAGE));
  fui::ListProps props;
  props.items = rows.data();
  props.count = rows.size();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}
