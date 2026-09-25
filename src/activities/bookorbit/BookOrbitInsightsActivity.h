#pragma once

#include <BookOrbitInsights.h>
#include <BookOrbitSyncState.h>

#include <cstdint>
#include <memory>
#include <string>

#include "activities/Activity.h"

struct Rect;

// BookOrbit reading insights: five pages drawn from the cached snapshot
// (Overview, Reader DNA, Goals & streaks, Calendar, Habits). Everything on
// screen comes from the on-card cache; a refresh runs on the background agent
// when the board has one, else as a foreground session that reboots back
// here. Charts are drawn with the renderer's own primitives into the normal
// framebuffer; nothing decodes or fetches during a paint.
//
// Buttons: Up/Down (and Left/Right) turn pages, Confirm refreshes. On the
// Calendar page Left/Right step months and Confirm enters day selection,
// where the directional buttons walk the grid and Confirm/Back leave it.
// Touch: swipe for pages, tap a day, tap the month arrows, tap Refresh.
class BookOrbitInsightsActivity final : public Activity {
 public:
  BookOrbitInsightsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Page : uint8_t { Overview, Dna, Goals, Calendar, Habits };
  static constexpr uint8_t PAGE_COUNT = 5;
  static constexpr uint8_t CALENDAR_MAX_MONTHS_BACK = 24;

  // Rect of a tappable region recorded during render (screen coordinates).
  struct Hit {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return w > 0 && px >= x && px < x + w && py >= y && py < y + h; }
  };

  // Data (heap: the calendar alone is ~600 bytes, the summary ~350).
  std::string account;
  std::unique_ptr<bookorbit::InsightsSummary> summary;
  std::unique_ptr<bookorbit::InsightsHabits> habits;
  std::unique_ptr<bookorbit::InsightsCalendar> calendar;
  bookorbit::SyncState syncState;

  Page page = Page::Overview;
  // Calendar month on screen (0 when no month can be named).
  uint16_t viewYear = 0;
  uint8_t viewMonth = 0;
  bool dayMode = false;
  uint8_t selectedDay = 1;
  // Background refresh in flight (agent), and the generation it started at.
  bool fetching = false;
  uint32_t seenGeneration = 0;

  // Touch targets from the last render.
  Hit hitRefresh, hitPrevMonth, hitNextMonth, hitPageLabel;
  Hit hitGrid;  // whole calendar grid; cell math resolves the day
  int gridCellW = 0, gridCellH = 0;
  uint8_t gridFirstWeekday = 0;

  void loadAll();
  void loadCalendar();
  bool currentMonth(uint16_t& year, uint8_t& month) const;
  uint32_t viewMonthCode() const { return static_cast<uint32_t>(viewYear) * 100u + viewMonth; }
  void stepMonth(int delta);
  void stepPage(int delta);
  void refresh();
  void leave();
  bool handleTouch();
  bool handleButtons();

  // Rendering
  void renderPage(const Rect& body);
  void renderOverview(const Rect& body);
  void renderDna(const Rect& body);
  void renderGoals(const Rect& body);
  void renderCalendar(const Rect& body);
  void renderHabits(const Rect& body);
  void renderEmpty(const Rect& body, const char* message);
  const char* emptyMessage() const;
  void formatStatus(char* out, size_t cap) const;
  const char* pageTitle(Page p) const;
  // Primitives shared by the pages.
  int drawHeading(int x, int y, int width, const char* text) const;
  int drawLine(int x, int y, int width, const char* text, bool bold = false, bool small = false) const;
  void drawProgressBar(int x, int y, int width, int height, int pct) const;
  void drawColumns(int x, int y, int width, int height, const uint32_t* values, int count, const char* const* labels,
                   int labelEvery, int highlight) const;
  void drawStreakStrip(int x, int y, int width, uint8_t bits) const;
  int lineHeight(bool small = false) const;
};
