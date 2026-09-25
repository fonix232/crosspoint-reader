#include "BookOrbitQueueActivity.h"

#include <BookOrbitCatalogCache.h>
#include <BookOrbitCredentialStore.h>
#include <FreeInkUIIcon.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "BookOrbitDownloadActivity.h"
#include "BookOrbitUi.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "bookorbit/BookOrbitAgent.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"

namespace fui = freeink::ui;

BookOrbitQueueActivity::BookOrbitQueueActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("BookOrbitQueue", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

void BookOrbitQueueActivity::onEnter() {
  account = BOOKORBIT_STORE.getAccountKey();
  reload();
  seenGeneration = BOOKORBIT_AGENT.generation();
  UiListActivity::onEnter();
}

bool BookOrbitQueueActivity::hasDownloadNowRow() const {
  return queue.queuedCount() > 0 && (!BOOKORBIT_AGENT.available() || !BOOKORBIT_AGENT.busy());
}

int BookOrbitQueueActivity::listCount() const {
  return static_cast<int>(queue.items().size()) + (hasDownloadNowRow() ? 1 : 0);
}

const char* BookOrbitQueueActivity::headerTitle() const { return tr(STR_BOOKORBIT_QUEUE); }

// Titles come from the catalogue; a book that left it keeps its id as label.
void BookOrbitQueueActivity::reload() {
  queue.load(account);
  titles.clear();
  values.clear();
  bookorbit::CatalogCache cache;
  const bool haveCatalog = cache.open(account);
  bookorbit::CatalogRecord record;
  for (const auto& item : queue.items()) {
    std::string title;
    if (haveCatalog) {
      const uint16_t ordinal = cache.ordinalForBook(item.bookId);
      if (ordinal != 0xFFFF && cache.readRecord(ordinal, record)) title.assign(record.titleView());
    }
    if (title.empty()) {
      char buf[16];
      snprintf(buf, sizeof(buf), "#%lu", static_cast<unsigned long>(item.bookId));
      title = buf;
    }
    titles.push_back(std::move(title));
    values.emplace_back();
  }
}

void BookOrbitQueueActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int count = listCount();
  if (count == 0) {
    screen.centeredText(tr(STR_BOOKORBIT_QUEUE_EMPTY));
    return;
  }
  rows.assign(static_cast<size_t>(count), fui::ListItem{});
  int row = 0;
  if (hasDownloadNowRow()) {
    rows[0].label = tr(STR_BOOKORBIT_QUEUE_DOWNLOAD_NOW);
    rows[0].icon = fui::bitmapFromIcon(icon_download_24);
    rows[0].actionValue = 0;
    row = 1;
  }
  auto& agent = BOOKORBIT_AGENT;
  const uint32_t active = agent.available() ? agent.downloadingBookId() : 0;
  for (size_t i = 0; i < queue.items().size(); i++, row++) {
    const auto& item = queue.items()[i];
    std::string& value = values[i];
    if (item.bookId == active) {
      const uint32_t total = agent.phaseB();
      const int pct = total > 0 ? static_cast<int>(static_cast<uint64_t>(agent.phaseA()) * 100 / total) : 0;
      char buf[32];
      snprintf(buf, sizeof(buf), tr(STR_BOOKORBIT_DOWNLOADING_PCT), pct);
      value = buf;
    } else if (item.state == bookorbit::QueueState::Failed) {
      value = std::string(tr(STR_BOOKORBIT_QUEUE_FAILED)) + ": " +
              bookOrbitErrorText(static_cast<bookorbit::ClientError>(item.error));
    } else {
      value = tr(STR_BOOKORBIT_QUEUED);
    }
    rows[static_cast<size_t>(row)].label = titles[i].c_str();
    rows[static_cast<size_t>(row)].subtitle = value.c_str();
    rows[static_cast<size_t>(row)].icon = fui::bitmapFromIcon(icon_book_24);
    rows[static_cast<size_t>(row)].actionValue = static_cast<int16_t>(row);
  }

  fui::ListProps props;
  props.items = rows.data();
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}

void BookOrbitQueueActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (hasDownloadNowRow() && index == 0) {
    downloadNow();
    return;
  }
  const int i = itemIndexOf(index);
  if (i < 0 || i >= static_cast<int>(queue.items().size())) return;
  const auto& item = queue.items()[static_cast<size_t>(i)];
  if (item.state == bookorbit::QueueState::Failed) {
    retry(item.bookId);
  } else {
    promptRemove(item.bookId, titles[static_cast<size_t>(i)]);
  }
}

void BookOrbitQueueActivity::onRowLongPress(const int index) {
  const int i = itemIndexOf(index);
  if (i < 0 || i >= static_cast<int>(queue.items().size())) return;
  promptRemove(queue.items()[static_cast<size_t>(i)].bookId, titles[static_cast<size_t>(i)]);
}

void BookOrbitQueueActivity::promptRemove(const uint32_t bookId, const std::string& title) {
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_QUEUE_REMOVE), title);
  if (!confirmation) return;
  startActivityForResult(std::move(confirmation), [this, bookId](const ActivityResult& result) {
    if (!result.isCancelled) {
      if (BOOKORBIT_AGENT.available()) {
        BOOKORBIT_AGENT.cancelDownload(bookId);
      } else {
        queue.load(account);
        queue.remove(bookId);
        queue.save();
      }
      {
        RenderLock lock(*this);
        reload();
        auto& nav = activeNav();
        const int count = listCount();
        if (nav.selected >= count) nav.selected = count > 0 ? count - 1 : 0;
        nav.followOnBuild = true;
      }
    }
    closeRouting();
    requestUpdate(true);
  });
}

void BookOrbitQueueActivity::retry(const uint32_t bookId) {
  if (BOOKORBIT_AGENT.available()) {
    BOOKORBIT_AGENT.requestDownload(bookId);
  } else {
    queue.load(account);
    queue.add(bookId);
    queue.save();
  }
  {
    RenderLock lock(*this);
    reload();
  }
  requestUpdate();
}

// Retry a waiting queue, using a foreground session on boards without the worker.
void BookOrbitQueueActivity::downloadNow() {
  if (BOOKORBIT_AGENT.available()) {
    BOOKORBIT_AGENT.resumeQueue();
    return;
  }
  activityManager.replaceActivity(
      std::make_unique<BookOrbitDownloadActivity>(renderer, mappedInput, BookOrbitDownloadActivity::QUEUE, ""));
}

// Follows the agent: reload on its generation and repaint the live row.
bool BookOrbitQueueActivity::handleCustomInput() {
  auto& agent = BOOKORBIT_AGENT;
  if (!agent.available()) return false;
  const unsigned long now = millis();
  if (agent.generation() != seenGeneration) {
    seenGeneration = agent.generation();
    {
      RenderLock lock(*this);
      reload();
    }
    lastPaintMs = now;
    requestUpdate();
  } else if (agent.downloadingBookId() != 0 && now - lastPaintMs > PROGRESS_PAINT_MS) {
    lastPaintMs = now;
    requestUpdate();
  }
  return false;
}

void BookOrbitQueueActivity::drawFooter() {
  const int count = listCount();
  const char* confirm = "";
  if (count > 0) {
    const int i = itemIndexOf(activeNav().selected);
    if (hasDownloadNowRow() && activeNav().selected == 0) {
      confirm = tr(STR_DOWNLOAD);
    } else if (i >= 0 && i < static_cast<int>(queue.items().size())) {
      confirm = queue.items()[static_cast<size_t>(i)].state == bookorbit::QueueState::Failed
                    ? tr(STR_BOOKORBIT_QUEUE_RETRY)
                    : tr(STR_BOOKORBIT_QUEUE_REMOVE_SHORT);
    }
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
