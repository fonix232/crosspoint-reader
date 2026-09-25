#pragma once

#include <BookOrbitDownloadQueue.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// The account's download queue: what is downloading, waiting, or failed.
// Confirm on a waiting row removes it (after a prompt), on a failed row
// re-queues it; holding a row removes it. Boards with the background agent
// see live progress here. An idle queue gets a "Download now" action; boards
// without the worker drain it in a foreground session.
class BookOrbitQueueActivity final : public UiListActivity {
 public:
  explicit BookOrbitQueueActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  static constexpr unsigned long PROGRESS_PAINT_MS = 1500;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  const char* headerTitle() const override;
  bool handleCustomInput() override;
  void drawFooter() override;

  // Row 0 retries a waiting queue; the queue items follow.
  bool hasDownloadNowRow() const;
  int itemIndexOf(int row) const { return hasDownloadNowRow() ? row - 1 : row; }
  void reload();
  void promptRemove(uint32_t bookId, const std::string& title);
  void retry(uint32_t bookId);
  void downloadNow();

  std::string account;
  bookorbit::DownloadQueue queue;
  std::vector<std::string> titles;
  std::vector<std::string> values;
  std::vector<freeink::ui::ListItem> rows;
  uint32_t seenGeneration = 0;
  unsigned long lastPaintMs = 0;
};
