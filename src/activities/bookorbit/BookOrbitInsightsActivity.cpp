#include "BookOrbitInsightsActivity.h"

#include <BookOrbitCredentialStore.h>
#include <BookOrbitInsightsCache.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "BookOrbitSyncActivity.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "bookorbit/BookOrbitAgent.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char TAG[] = "BOINS";
constexpr int SIDE = 16;   // content inset from the safe area
constexpr int GAP = 8;     // vertical breathing room between blocks
constexpr int BAR_H = 14;  // progress / DNA bar height
// Repaint throttle while the agent works (e-ink).

const StrId WEEKDAY_SHORT[7] = {StrId::STR_WEEKDAY_SHORT_SUN, StrId::STR_WEEKDAY_SHORT_MON,
                                StrId::STR_WEEKDAY_SHORT_TUE, StrId::STR_WEEKDAY_SHORT_WED,
                                StrId::STR_WEEKDAY_SHORT_THU, StrId::STR_WEEKDAY_SHORT_FRI,
                                StrId::STR_WEEKDAY_SHORT_SAT};
const StrId WEEKDAY_LONG[7] = {StrId::STR_WEEKDAY_SUN, StrId::STR_WEEKDAY_MON, StrId::STR_WEEKDAY_TUE,
                               StrId::STR_WEEKDAY_WED, StrId::STR_WEEKDAY_THU, StrId::STR_WEEKDAY_FRI,
                               StrId::STR_WEEKDAY_SAT};
const StrId MONTH_NAMES[12] = {StrId::STR_MONTH_JAN, StrId::STR_MONTH_FEB, StrId::STR_MONTH_MAR, StrId::STR_MONTH_APR,
                               StrId::STR_MONTH_MAY, StrId::STR_MONTH_JUN, StrId::STR_MONTH_JUL, StrId::STR_MONTH_AUG,
                               StrId::STR_MONTH_SEP, StrId::STR_MONTH_OCT, StrId::STR_MONTH_NOV, StrId::STR_MONTH_DEC};

// Weekday (Sunday = 0) of a YYYY-MM-DD key; 0 when malformed.
uint8_t weekdayOfDate(const char* date) {
  if (strlen(date) != 10) return 0;
  const int year = atoi(date);
  const int month = atoi(date + 5);
  const int day = atoi(date + 8);
  if (month < 1 || month > 12 || day < 1) return 0;
  return static_cast<uint8_t>(
      (bookorbit::weekdayOfFirst(static_cast<uint16_t>(year), static_cast<uint8_t>(month)) + day - 1) % 7);
}

// Intensity band for the calendar legend: 0 none, 1 <15m, 2 15-45m, 3 45-90m, 4 90m+.
uint8_t bandFor(const uint32_t seconds) {
  if (seconds == 0) return 0;
  if (seconds < 15 * 60) return 1;
  if (seconds < 45 * 60) return 2;
  if (seconds < 90 * 60) return 3;
  return 4;
}
}  // namespace

BookOrbitInsightsActivity::BookOrbitInsightsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BookOrbitInsights", renderer, mappedInput) {}

void BookOrbitInsightsActivity::onEnter() {
  Activity::onEnter();
  account = BOOKORBIT_STORE.getAccountKey();
  summary = makeUniqueNoThrow<bookorbit::InsightsSummary>();
  habits = makeUniqueNoThrow<bookorbit::InsightsHabits>();
  calendar = makeUniqueNoThrow<bookorbit::InsightsCalendar>();
  if (!summary || !habits || !calendar) {
    LOG_ERR(TAG, "OOM: insights records");
    summary.reset();
    habits.reset();
    calendar.reset();
  }
  loadAll();
  if (viewYear == 0) currentMonth(viewYear, viewMonth);
  loadCalendar();
  seenGeneration = BOOKORBIT_AGENT.generation();
  requestUpdate();
}

void BookOrbitInsightsActivity::onExit() {
  Activity::onExit();
  summary.reset();
  habits.reset();
  calendar.reset();
}

// --- data --------------------------------------------------------------------------

void BookOrbitInsightsActivity::loadAll() {
  bookorbit::loadSyncState(account, syncState);
  if (summary && !bookorbit::loadInsightsSummary(account, *summary)) *summary = bookorbit::InsightsSummary{};
  if (habits && !bookorbit::loadInsightsHabits(account, *habits)) *habits = bookorbit::InsightsHabits{};
}

void BookOrbitInsightsActivity::loadCalendar() {
  if (!calendar) return;
  if (viewYear == 0 || !bookorbit::loadInsightsCalendar(account, viewYear, viewMonth, *calendar)) {
    *calendar = bookorbit::InsightsCalendar{};
  }
  if (calendar->valid && (selectedDay < 1 || selectedDay > calendar->dayCount)) {
    selectedDay = calendar->dayCount > 0 ? calendar->dayCount : 1;
  }
}

// The wall clock names the current month; without one, the snapshot's own
// generation time is the next best anchor.
bool BookOrbitInsightsActivity::currentMonth(uint16_t& year, uint8_t& month) const {
  uint32_t at = bookorbit::nowIfClockValid();
  if (at == 0 && summary && summary->valid) at = summary->generatedAt;
  if (at == 0) return false;
  const time_t t = static_cast<time_t>(at);
  struct tm parts{};
  gmtime_r(&t, &parts);
  year = static_cast<uint16_t>(parts.tm_year + 1900);
  month = static_cast<uint8_t>(parts.tm_mon + 1);
  return true;
}

void BookOrbitInsightsActivity::stepMonth(const int delta) {
  if (viewYear == 0) return;
  int year = viewYear;
  int month = viewMonth + delta;
  while (month < 1) {
    month += 12;
    year--;
  }
  while (month > 12) {
    month -= 12;
    year++;
  }
  uint16_t nowYear = 0;
  uint8_t nowMonth = 0;
  if (currentMonth(nowYear, nowMonth)) {
    const int ahead = (year - nowYear) * 12 + (month - nowMonth);
    if (ahead > 0 || -ahead > CALENDAR_MAX_MONTHS_BACK) return;
  }
  viewYear = static_cast<uint16_t>(year);
  viewMonth = static_cast<uint8_t>(month);
  dayMode = false;
  selectedDay = 1;
  loadCalendar();
  // An uncached month is fetched in the background when the agent is up; on
  // other boards Refresh runs a foreground session for it.
  if (!calendar->valid && BOOKORBIT_AGENT.available() && !BOOKORBIT_AGENT.busy()) {
    BOOKORBIT_AGENT.requestInsights(viewMonthCode());
    fetching = true;
  }
  requestUpdate();
}

void BookOrbitInsightsActivity::stepPage(const int delta) {
  int next = static_cast<int>(page) + delta;
  if (next < 0) next = PAGE_COUNT - 1;
  if (next >= PAGE_COUNT) next = 0;
  page = static_cast<Page>(next);
  dayMode = false;
  requestUpdate();
}

void BookOrbitInsightsActivity::refresh() {
  if (!BOOKORBIT_STORE.isConfigured()) return;
  if (BOOKORBIT_AGENT.available()) {
    if (!BOOKORBIT_AGENT.busy()) BOOKORBIT_AGENT.requestInsights(viewMonthCode(), /*syncReading=*/true);
    fetching = true;
    requestUpdate();
    return;
  }
  // The session reboots on exit and lands back here.
  activityManager.replaceActivity(std::make_unique<BookOrbitSyncActivity>(
      renderer, mappedInput, BookOrbitSyncActivity::ReturnTo::Insights, viewMonthCode()));
}

void BookOrbitInsightsActivity::leave() { onGoHome(HomeMenuItem::INSIGHTS); }

// --- input -------------------------------------------------------------------------

void BookOrbitInsightsActivity::loop() {
  auto& agent = BOOKORBIT_AGENT;
  if (agent.available()) {
    const bool dataChanged = agent.generation() != seenGeneration;
    const bool fetchFinished = fetching && !agent.busy();
    if (dataChanged || fetchFinished) {
      seenGeneration = agent.generation();
      if (fetchFinished) fetching = false;
      loadAll();
      loadCalendar();
      requestUpdate();
    }
  }
  if (handleTouch()) return;
  handleButtons();
}

bool BookOrbitInsightsActivity::handleTouch() {
  if (mappedInput.wasHomeGesture()) {
    leave();
    return true;
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) {
    stepPage(1);
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) {
    stepPage(-1);
    return true;
  }
  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return false;
  if (hitRefresh.contains(x, y)) {
    refresh();
    return true;
  }
  if (hitPageLabel.contains(x, y)) {
    stepPage(1);
    return true;
  }
  if (page == Page::Calendar) {
    if (hitPrevMonth.contains(x, y)) {
      stepMonth(-1);
      return true;
    }
    if (hitNextMonth.contains(x, y)) {
      stepMonth(1);
      return true;
    }
    if (hitGrid.contains(x, y) && calendar && calendar->valid && gridCellW > 0 && gridCellH > 0) {
      const int col = (x - hitGrid.x) / gridCellW;
      const int row = (y - hitGrid.y) / gridCellH;
      const int day = row * 7 + col - gridFirstWeekday + 1;
      if (day >= 1 && day <= calendar->dayCount) {
        selectedDay = static_cast<uint8_t>(day);
        dayMode = true;
        requestUpdate();
      }
      return true;
    }
  }
  return false;
}

bool BookOrbitInsightsActivity::handleButtons() {
  using Button = MappedInputManager::Button;
  const bool back = mappedInput.wasReleased(Button::Back);
  const bool confirm = mappedInput.wasReleased(Button::Confirm);
  const bool up = mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::PageBack);
  const bool down = mappedInput.wasReleased(Button::Down) || mappedInput.wasReleased(Button::PageForward);
  const bool left = mappedInput.wasReleased(Button::Left);
  const bool right = mappedInput.wasReleased(Button::Right);

  if (page == Page::Calendar && dayMode && calendar && calendar->valid) {
    if (back || confirm) {
      dayMode = false;
      requestUpdate();
      return true;
    }
    int day = selectedDay;
    if (up) day -= 7;
    if (down) day += 7;
    if (left) day -= 1;
    if (right) day += 1;
    if (day != selectedDay) {
      if (day < 1) day = 1;
      if (day > calendar->dayCount) day = calendar->dayCount;
      selectedDay = static_cast<uint8_t>(day);
      requestUpdate();
      return true;
    }
    return false;
  }
  if (back) {
    leave();
    return true;
  }
  if (page == Page::Calendar) {
    if (left) {
      stepMonth(-1);
      return true;
    }
    if (right) {
      stepMonth(1);
      return true;
    }
    if (confirm) {
      if (calendar && calendar->valid && calendar->dayCount > 0) {
        dayMode = true;
        requestUpdate();
      } else {
        refresh();
      }
      return true;
    }
  } else if (confirm) {
    refresh();
    return true;
  }
  if (up || left) {
    stepPage(-1);
    return true;
  }
  if (down || right) {
    stepPage(1);
    return true;
  }
  return false;
}

// --- rendering ---------------------------------------------------------------------

const char* BookOrbitInsightsActivity::pageTitle(const Page p) const {
  switch (p) {
    case Page::Overview:
      return tr(STR_INSIGHTS_PAGE_OVERVIEW);
    case Page::Dna:
      return tr(STR_INSIGHTS_PAGE_DNA);
    case Page::Goals:
      return tr(STR_INSIGHTS_PAGE_GOALS);
    case Page::Calendar:
      return tr(STR_INSIGHTS_PAGE_CALENDAR);
    case Page::Habits:
      return tr(STR_INSIGHTS_PAGE_HABITS);
  }
  return "";
}

// "Updated 2026-04-08 10:00" / "Fetching..." / "Not synced yet"; the freshness
// of what is on screen, never of the server's aggregates.
void BookOrbitInsightsActivity::formatStatus(char* out, const size_t cap) const {
  if (fetching) {
    snprintf(out, cap, "%s", tr(STR_INSIGHTS_FETCHING));
    return;
  }
  if (syncState.insightsAt == 0) {
    snprintf(out, cap, "%s", tr(STR_INSIGHTS_NEVER_SYNCED));
    return;
  }
  const time_t when = static_cast<time_t>(syncState.insightsAt);
  struct tm local{};
  localtime_r(&when, &local);
  char stamp[24];
  strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &local);
  snprintf(out, cap, tr(STR_INSIGHTS_UPDATED), stamp);
}

int BookOrbitInsightsActivity::lineHeight(const bool small) const {
  return renderer.getLineHeight(small ? SMALL_FONT_ID : UI_10_FONT_ID);
}

int BookOrbitInsightsActivity::drawHeading(const int x, const int y, const int width, const char* text) const {
  renderer.drawText(UI_12_FONT_ID, x, y, text, true, EpdFontFamily::BOLD);
  const int h = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawLine(x, y + h + 1, x + width, y + h + 1, true);
  return y + h + GAP;
}

int BookOrbitInsightsActivity::drawLine(const int x, const int y, const int width, const char* text, const bool bold,
                                        const bool small) const {
  const int font = small ? SMALL_FONT_ID : UI_10_FONT_ID;
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const std::string fitted = renderer.truncatedText(font, text, width, style);
  renderer.drawText(font, x, y, fitted.c_str(), true, style);
  return y + renderer.getLineHeight(font);
}

void BookOrbitInsightsActivity::drawProgressBar(const int x, const int y, const int width, const int height,
                                                int pct) const {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  renderer.drawRect(x, y, width, height, true);
  const int fill = (width - 4) * pct / 100;
  if (fill > 0) renderer.fillRect(x + 2, y + 2, fill, height - 4, true);
}

// Vertical bars scaled to the largest value; `highlight` (index) is drawn
// solid, the rest dithered dark, a zero as a baseline tick only. Labels go
// under every `labelEvery`-th bar.
void BookOrbitInsightsActivity::drawColumns(const int x, const int y, const int width, const int height,
                                            const uint32_t* values, const int count, const char* const* labels,
                                            const int labelEvery, const int highlight) const {
  if (count <= 0) return;
  uint32_t peak = 0;
  for (int i = 0; i < count; i++) peak = std::max(peak, values[i]);
  const int labelH = labels ? lineHeight(true) : 0;
  const int chartH = height - labelH - 2;
  const int slot = width / count;
  const int barW = std::max(2, slot - (count > 12 ? 2 : 6));
  const int baseline = y + chartH;
  renderer.drawLine(x, baseline, x + slot * count, baseline, true);
  for (int i = 0; i < count; i++) {
    const int bx = x + i * slot + (slot - barW) / 2;
    int barH = peak == 0 ? 0 : static_cast<int>(static_cast<uint64_t>(values[i]) * (chartH - 2) / peak);
    if (values[i] > 0 && barH < 2) barH = 2;
    if (barH > 0) {
      if (i == highlight) {
        renderer.fillRect(bx, baseline - barH, barW, barH, true);
      } else {
        renderer.fillRectDither(bx, baseline - barH, barW, barH, Color::DarkGray);
        renderer.drawRect(bx, baseline - barH, barW, barH, true);
      }
    }
    if (labels && labelEvery > 0 && i % labelEvery == 0 && labels[i]) {
      const int tw = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      renderer.drawText(SMALL_FONT_ID, x + i * slot + (slot - tw) / 2, baseline + 2, labels[i]);
    }
  }
}

// Seven boxes, oldest left; a filled box is a day with reading.
void BookOrbitInsightsActivity::drawStreakStrip(const int x, const int y, const int width, const uint8_t bits) const {
  const int slot = width / 7;
  const int box = std::min(slot - 4, 28);
  for (int i = 0; i < 7; i++) {
    const int bx = x + i * slot + (slot - box) / 2;
    if (bits & (1u << i)) {
      renderer.fillRect(bx, y, box, box, true);
    } else {
      renderer.drawRect(bx, y, box, box, true);
    }
  }
}

// Why a page is empty: no snapshot yet, a server without the routes, or the
// KOReader user's insights toggle being off.
const char* BookOrbitInsightsActivity::emptyMessage() const {
  if (syncState.insightsDisabled) return tr(STR_INSIGHTS_DISABLED);
  if (syncState.insightsUnsupported) return tr(STR_INSIGHTS_UNSUPPORTED);
  return tr(STR_INSIGHTS_EMPTY);
}

void BookOrbitInsightsActivity::renderEmpty(const Rect& body, const char* message) {
  UITheme::drawCenteredWrappedText(renderer, Rect{body.x + SIDE, body.y, body.width - SIDE * 2, body.height},
                                   UI_10_FONT_ID, message, 4);
}

void BookOrbitInsightsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  char pageNo[8];
  snprintf(pageNo, sizeof(pageNo), "%u/%u", static_cast<unsigned>(static_cast<int>(page) + 1),
           static_cast<unsigned>(PAGE_COUNT));
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_BOOKORBIT_INSIGHTS), pageNo);

  // Sub-header: page name (tap: next page) and the snapshot's freshness or a
  // Refresh target on touch boards.
  const int subTop = screen.y + metrics.topPadding + metrics.headerHeight;
  const int subH = lineHeight() + 12;
  char status[64];
  formatStatus(status, sizeof(status));
  const char* rightLabel = mappedInput.hasTouch() && !fetching ? tr(STR_INSIGHTS_REFRESH) : status;
  GUI.drawSubHeader(renderer, Rect{screen.x, subTop, screen.width, subH}, pageTitle(page), rightLabel);
  hitPageLabel = Hit{screen.x, subTop, screen.width / 2, subH};
  hitRefresh = mappedInput.hasTouch() ? Hit{screen.x + screen.width / 2, subTop, screen.width / 2, subH} : Hit{};

  const int bodyTop = subTop + subH + GAP;
  const int footerH = lineHeight(true) + 4;
  const Rect body{screen.x, bodyTop, screen.width, screen.y + screen.height - bodyTop - footerH};
  renderPage(body);

  // Freshness line above the hints on touch boards (the sub-header carried the
  // Refresh target instead), and the timezone note where dates are shown.
  if (mappedInput.hasTouch()) {
    renderer.drawText(SMALL_FONT_ID, screen.x + SIDE, screen.y + screen.height - footerH, status);
  }

  const char* confirmLabel = tr(STR_INSIGHTS_REFRESH);
  const char* prevLabel = tr(STR_INSIGHTS_PAGE);
  const char* nextLabel = tr(STR_INSIGHTS_PAGE);
  const char* backLabel = tr(STR_HOME);
  if (page == Page::Calendar) {
    if (dayMode) {
      backLabel = tr(STR_BACK);
      confirmLabel = tr(STR_DONE);
      prevLabel = tr(STR_INSIGHTS_DAY_MOVE);
      nextLabel = tr(STR_INSIGHTS_DAY_MOVE);
    } else if (calendar && calendar->valid) {
      confirmLabel = tr(STR_INSIGHTS_SELECT_DAY);
    }
  }
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, prevLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BookOrbitInsightsActivity::renderPage(const Rect& body) {
  hitPrevMonth = hitNextMonth = hitGrid = Hit{};
  if (!summary) {
    renderEmpty(body, tr(STR_BOOKORBIT_ERR_MEMORY));
    return;
  }
  switch (page) {
    case Page::Overview:
      renderOverview(body);
      break;
    case Page::Dna:
      renderDna(body);
      break;
    case Page::Goals:
      renderGoals(body);
      break;
    case Page::Calendar:
      renderCalendar(body);
      break;
    case Page::Habits:
      renderHabits(body);
      break;
  }
}

void BookOrbitInsightsActivity::renderOverview(const Rect& body) {
  const bookorbit::InsightsSummary& s = *summary;
  if (!s.valid) {
    renderEmpty(body, emptyMessage());
    return;
  }
  const int x = body.x + SIDE;
  const int w = body.width - SIDE * 2;
  int y = body.y;
  char buf[96];
  char dur[16];

  // This week: total, active days, seven bars.
  bookorbit::formatDurationShort(s.last7DaysSeconds, dur, sizeof(dur));
  snprintf(buf, sizeof(buf), "%s: %s", tr(STR_INSIGHTS_THIS_WEEK), dur);
  y = drawHeading(x, y, w, buf);
  snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_ACTIVE_DAYS), static_cast<unsigned>(s.last7DaysActive));
  y = drawLine(x, y, w, buf, false, true);
  if (s.weekCount > 0) {
    uint32_t values[7] = {};
    const char* labels[7] = {};
    for (uint8_t i = 0; i < s.weekCount; i++) {
      values[i] = s.week[i].seconds;
      labels[i] = I18N.get(WEEKDAY_SHORT[weekdayOfDate(s.week[i].date)]);
    }
    const int chartH = std::min(body.height / 4, 110);
    drawColumns(x, y + 2, w, chartH, values, s.weekCount, labels, 1, s.weekCount - 1);
    y += chartH + GAP;
  }

  // Last 30 days.
  bookorbit::formatDurationShort(s.last30DaysSeconds, dur, sizeof(dur));
  snprintf(buf, sizeof(buf), "%s: %s", tr(STR_INSIGHTS_LAST_30), dur);
  y = drawLine(x, y, w, buf, true);
  snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_ACTIVE_DAYS), static_cast<unsigned>(s.last30DaysActive));
  y = drawLine(x, y, w, buf, false, true) + GAP;

  // Streak.
  snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_DAYS_FMT), static_cast<unsigned>(s.currentStreak));
  char line[128];
  snprintf(line, sizeof(line), "%s: %s", tr(STR_INSIGHTS_STREAK), buf);
  y = drawLine(x, y, w, line, true);
  drawStreakStrip(x, y + 2, w / 2, s.lastSevenDays);
  snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_LONGEST_FMT), static_cast<unsigned>(s.longestStreak));
  renderer.drawText(SMALL_FONT_ID, x + w / 2 + GAP, y + 2, buf);
  y += std::min(w / 14 - 4, 28) + GAP * 2;

  // Goal.
  if (s.goalBooks > 0) {
    snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_GOAL_FMT), static_cast<unsigned>(s.goalCompleted),
             static_cast<unsigned>(s.goalBooks));
    snprintf(line, sizeof(line), "%u: %s", static_cast<unsigned>(s.goalYear), buf);
    y = drawLine(x, y, w, line, true);
    drawProgressBar(x, y + 2, w, BAR_H, static_cast<int>(100u * s.goalCompleted / s.goalBooks));
    y += BAR_H + GAP * 2;
  } else {
    y = drawLine(x, y, w, tr(STR_INSIGHTS_NO_GOAL)) + GAP;
  }

  // Reader DNA one-liner and the account's book counts.
  if (s.booksAnalyzed > 0 && s.archetype[0] != '\0') {
    snprintf(line, sizeof(line), "%s: %s", tr(STR_INSIGHTS_PAGE_DNA), s.archetype);
    y = drawLine(x, y, w, line);
  }
  snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_LIBRARY_FMT), static_cast<unsigned>(s.trackedBooks),
           static_cast<unsigned>(s.inProgressBooks), static_cast<unsigned>(s.completedBooks));
  if (y + lineHeight(true) <= body.y + body.height) drawLine(x, y, w, buf, false, true);
}

void BookOrbitInsightsActivity::renderDna(const Rect& body) {
  const bookorbit::InsightsSummary& s = *summary;
  if (!s.valid) {
    renderEmpty(body, emptyMessage());
    return;
  }
  const int x = body.x + SIDE;
  const int w = body.width - SIDE * 2;
  int y = body.y;
  char buf[96];

  const bool established = s.booksAnalyzed > 0 && s.archetype[0] != '\0';
  y = drawHeading(x, y, w, established ? s.archetype : tr(STR_INSIGHTS_ARCHETYPE_NONE));
  snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_BOOKS_ANALYZED), static_cast<unsigned>(s.booksAnalyzed));
  y = drawLine(x, y, w, buf, false, true) + GAP * 2;

  struct Row {
    const char* name;
    uint8_t score;
    const char* label;
    bool available;
  };
  const Row rows[5] = {
      {tr(STR_INSIGHTS_DNA_LENGTH), s.lengthScore, s.lengthLabel, true},
      {tr(STR_INSIGHTS_DNA_VARIETY), s.varietyScore, s.varietyLabel, true},
      {tr(STR_INSIGHTS_DNA_RHYTHM), s.rhythmScore, s.rhythmLabel, true},
      {tr(STR_INSIGHTS_DNA_TIME), s.timeScore, s.timeLabel, true},
      {tr(STR_INSIGHTS_DNA_SPEED), s.speedScore, s.speedLabel, s.speedAvailable()},
  };
  const int rowH = lineHeight() + BAR_H + GAP * 2;
  for (const Row& row : rows) {
    if (y + rowH > body.y + body.height) break;
    renderer.drawText(UI_10_FONT_ID, x, y, row.name, true, EpdFontFamily::BOLD);
    const char* value = row.available && row.label[0] != '\0' ? row.label : tr(STR_INSIGHTS_NA);
    const int vw = renderer.getTextWidth(UI_10_FONT_ID, value);
    renderer.drawText(UI_10_FONT_ID, x + w - vw, y, value);
    y += lineHeight() + 2;
    if (row.available && established) {
      drawProgressBar(x, y, w, BAR_H, row.score);
    } else {
      // Unavailable or evidence-free: an empty frame, not a measured zero.
      renderer.drawRect(x, y, w, BAR_H, true);
    }
    y += BAR_H + GAP * 2;
  }
}

void BookOrbitInsightsActivity::renderGoals(const Rect& body) {
  const bookorbit::InsightsSummary& s = *summary;
  if (!s.valid) {
    renderEmpty(body, emptyMessage());
    return;
  }
  const int x = body.x + SIDE;
  const int w = body.width - SIDE * 2;
  int y = body.y;
  char buf[96];

  snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_GOAL_YEAR), static_cast<unsigned>(s.goalYear));
  y = drawHeading(x, y, w, buf);
  if (s.goalBooks > 0) {
    snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_GOAL_FMT), static_cast<unsigned>(s.goalCompleted),
             static_cast<unsigned>(s.goalBooks));
    y = drawLine(x, y, w, buf, true);
    drawProgressBar(x, y + 2, w, BAR_H + 6, static_cast<int>(100u * s.goalCompleted / s.goalBooks));
    y += BAR_H + 6 + GAP * 2;
  } else {
    y = drawLine(x, y, w, tr(STR_INSIGHTS_NO_GOAL));
    snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_COMPLETED_FMT), static_cast<unsigned>(s.goalCompleted));
    y = drawLine(x, y, w, buf, false, true) + GAP;
  }
  snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_PROJECTION), static_cast<unsigned>(s.projectedBooks),
           static_cast<unsigned>(s.daysRemaining));
  y = drawLine(x, y, w, buf);
  const char* trend = s.trend > 0   ? tr(STR_INSIGHTS_TREND_UP)
                      : s.trend < 0 ? tr(STR_INSIGHTS_TREND_DOWN)
                                    : tr(STR_INSIGHTS_TREND_STABLE);
  y = drawLine(x, y, w, trend, false, true) + GAP * 2;

  y = drawHeading(x, y, w, tr(STR_INSIGHTS_STREAK));
  snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_DAYS_FMT), static_cast<unsigned>(s.currentStreak));
  y = drawLine(x, y, w, buf, true);
  snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_LONGEST_FMT), static_cast<unsigned>(s.longestStreak));
  y = drawLine(x, y, w, buf, false, true) + GAP;
  y = drawLine(x, y, w, tr(STR_INSIGHTS_LAST_SEVEN), false, true);
  drawStreakStrip(x, y + 2, w, s.lastSevenDays);
  // Weekday letters under the strip when the week's dates are known.
  if (s.weekCount == 7) {
    const int slot = w / 7;
    const int box = std::min(slot - 4, 28);
    for (int i = 0; i < 7; i++) {
      const char* label = I18N.get(WEEKDAY_SHORT[weekdayOfDate(s.week[i].date)]);
      const int tw = renderer.getTextWidth(SMALL_FONT_ID, label);
      renderer.drawText(SMALL_FONT_ID, x + i * slot + (slot - tw) / 2, y + 2 + box + 2, label);
    }
  }
}

void BookOrbitInsightsActivity::renderCalendar(const Rect& body) {
  const int x = body.x + SIDE;
  const int w = body.width - SIDE * 2;
  int y = body.y;
  char buf[96];
  char dur[16];

  // Month title with arrows (touch targets: the arrow ends of the row).
  const int titleH = renderer.getLineHeight(UI_12_FONT_ID);
  if (viewYear != 0) {
    snprintf(buf, sizeof(buf), "%s %u", I18N.get(MONTH_NAMES[viewMonth - 1]), static_cast<unsigned>(viewYear));
  } else {
    snprintf(buf, sizeof(buf), "%s", tr(STR_INSIGHTS_PAGE_CALENDAR));
  }
  const int tw = renderer.getTextWidth(UI_12_FONT_ID, buf, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + (w - tw) / 2, y, buf, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x, y, "<", true, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + w - renderer.getTextWidth(UI_12_FONT_ID, ">", EpdFontFamily::BOLD), y, ">", true,
                    EpdFontFamily::BOLD);
  hitPrevMonth = Hit{body.x, y - 8, body.width / 4, titleH + 16};
  hitNextMonth = Hit{body.x + body.width * 3 / 4, y - 8, body.width / 4, titleH + 16};
  y += titleH + GAP;

  if (viewYear == 0 || !calendar || !calendar->valid) {
    const char* message = tr(STR_INSIGHTS_NOT_CACHED);
    if (syncState.insightsUnsupported || syncState.insightsDisabled) message = emptyMessage();
    if (fetching) message = tr(STR_INSIGHTS_FETCHING);
    renderEmpty(Rect{body.x, y, body.width, body.height - (y - body.y)}, message);
    return;
  }
  const bookorbit::InsightsCalendar& c = *calendar;

  // Weekday header, then the grid.
  const int cellW = w / 7;
  const int gridX = x + (w - cellW * 7) / 2;
  for (int i = 0; i < 7; i++) {
    const char* label = I18N.get(WEEKDAY_SHORT[i]);
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, gridX + i * cellW + (cellW - lw) / 2, y, label);
  }
  y += lineHeight(true) + 2;

  const uint8_t first = bookorbit::weekdayOfFirst(viewYear, viewMonth);
  const uint8_t monthDays = bookorbit::daysInMonth(viewYear, viewMonth);
  const int rows = (first + monthDays + 6) / 7;
  // Leave room under the grid for the summary lines and the legend.
  const int reserved = lineHeight() * 2 + lineHeight(true) * 2 + GAP * 2;
  const int cellH = std::max(20, std::min(cellW, (body.y + body.height - y - reserved) / rows));
  gridCellW = cellW;
  gridCellH = cellH;
  gridFirstWeekday = first;
  hitGrid = Hit{gridX, y, cellW * 7, cellH * rows};
  // The data watermark gets a double frame when it falls in this month.
  const bool throughHere =
      strlen(c.dataThrough) == 10 && atoi(c.dataThrough) == viewYear && atoi(c.dataThrough + 5) == viewMonth;
  const uint8_t throughDay = throughHere ? static_cast<uint8_t>(atoi(c.dataThrough + 8)) : 0;

  for (int day = 1; day <= monthDays; day++) {
    const int slot = first + day - 1;
    const int cx = gridX + (slot % 7) * cellW;
    const int cy = y + (slot / 7) * cellH;
    const bool covered = day <= c.dayCount;
    const uint8_t band = covered ? bandFor(c.seconds[day - 1]) : 0;
    // Fill by band; days past the data watermark stay blank (not zero).
    switch (band) {
      case 1:
        renderer.fillRectDither(cx + 1, cy + 1, cellW - 2, cellH - 2, Color::LightGray);
        break;
      case 2:
        renderer.fillRectDither(cx + 1, cy + 1, cellW - 2, cellH - 2, Color::DarkGray);
        break;
      case 3:
        renderer.fillRect(cx + 1, cy + 1, cellW - 2, cellH - 2, true);
        break;
      case 4:
        renderer.fillRect(cx + 1, cy + 1, cellW - 2, cellH - 2, true);
        renderer.drawRect(cx + 4, cy + 4, cellW - 8, cellH - 8, false);
        break;
      default:
        break;
    }
    if (covered) renderer.drawRect(cx, cy, cellW, cellH, true);
    if (day == throughDay) renderer.drawRect(cx + 1, cy + 1, cellW - 2, cellH - 2, true);
    if (dayMode && day == selectedDay) renderer.drawRect(cx - 1, cy - 1, cellW + 2, cellH + 2, 3, true);
    snprintf(buf, sizeof(buf), "%d", day);
    const int nw = renderer.getTextWidth(SMALL_FONT_ID, buf);
    renderer.drawText(SMALL_FONT_ID, cx + (cellW - nw) / 2, cy + (cellH - lineHeight(true)) / 2, buf, band < 3);
  }
  y += cellH * rows + GAP;

  if (dayMode) {
    // Selected day: date, minutes, and the per-source split.
    const uint32_t seconds = c.seconds[selectedDay - 1];
    bookorbit::formatDurationShort(seconds, dur, sizeof(dur));
    snprintf(buf, sizeof(buf), "%s %u: %s", I18N.get(MONTH_NAMES[viewMonth - 1]), static_cast<unsigned>(selectedDay),
             seconds > 0 ? dur : tr(STR_INSIGHTS_NO_READING));
    y = drawLine(x, y, w, buf, true);
    if (seconds > 0) {
      const StrId names[3] = {StrId::STR_INSIGHTS_SOURCE_BOOKORBIT, StrId::STR_INSIGHTS_SOURCE_KOREADER,
                              StrId::STR_INSIGHTS_SOURCE_KOBO};
      std::string split;
      for (int src = 0; src < 3; src++) {
        const uint32_t part = c.bySource[selectedDay - 1][src];
        if (part == 0) continue;
        bookorbit::formatDurationShort(part, dur, sizeof(dur));
        if (!split.empty()) split += ", ";
        split += I18N.get(names[src]);
        split += " ";
        split += dur;
      }
      if (!split.empty()) y = drawLine(x, y, w, split.c_str(), false, true);
    }
  } else {
    bookorbit::formatDurationShort(c.totalSeconds(), dur, sizeof(dur));
    snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_MONTH_TOTAL), dur, static_cast<unsigned>(c.activeDays()));
    y = drawLine(x, y, w, buf, true);
    const uint8_t busiest = c.busiestDay();
    if (busiest > 0) {
      bookorbit::formatDurationShort(c.seconds[busiest - 1], dur, sizeof(dur));
      snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_BUSIEST), static_cast<unsigned>(busiest), dur);
      y = drawLine(x, y, w, buf, false, true);
    }
  }
  // Legend, and the timezone the days are keyed in.
  const int legendY = body.y + body.height - lineHeight(true) * 2;
  drawLine(x, legendY, w, tr(STR_INSIGHTS_LEGEND), false, true);
  if (c.daysTimezone[0] != '\0') drawLine(x, legendY + lineHeight(true), w, c.daysTimezone, false, true);
}

void BookOrbitInsightsActivity::renderHabits(const Rect& body) {
  const bookorbit::InsightsHabits& h = *habits;
  if (!h.valid) {
    renderEmpty(body, emptyMessage());
    return;
  }
  const int x = body.x + SIDE;
  const int w = body.width - SIDE * 2;
  int y = body.y;
  char buf[96];

  snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_LAST_N_DAYS), static_cast<unsigned>(h.days));
  y = drawHeading(x, y, w, buf);

  const int chartH = (body.height - (y - body.y) - lineHeight() * 4 - GAP * 6) / 2;

  y = drawLine(x, y, w, tr(STR_INSIGHTS_WEEKDAYS), true);
  const char* dayLabels[7];
  for (int i = 0; i < 7; i++) dayLabels[i] = I18N.get(WEEKDAY_SHORT[i]);
  const bool anyWeekday = std::any_of(h.weekday, h.weekday + 7, [](uint32_t v) { return v > 0; });
  drawColumns(x, y + 2, w, chartH, h.weekday, 7, dayLabels, 1, anyWeekday ? h.bestWeekday() : -1);
  y += chartH + 2;
  if (anyWeekday) {
    snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_BEST_DAY), I18N.get(WEEKDAY_LONG[h.bestWeekday()]));
    y = drawLine(x, y, w, buf, false, true);
  }
  y += GAP * 2;

  y = drawLine(x, y, w, tr(STR_INSIGHTS_HOURS), true);
  const char* hourLabels[24] = {};
  hourLabels[0] = "0";
  hourLabels[6] = "6";
  hourLabels[12] = "12";
  hourLabels[18] = "18";
  const bool anyHour = std::any_of(h.hour, h.hour + 24, [](uint32_t v) { return v > 0; });
  drawColumns(x, y + 2, w, chartH, h.hour, 24, hourLabels, 6, anyHour ? h.peakHour() : -1);
  y += chartH + 2;
  if (anyHour) {
    const unsigned peak = h.peakHour();
    snprintf(buf, sizeof(buf), tr(STR_INSIGHTS_PEAK_HOURS), peak, (peak + 1) % 24);
    drawLine(x, y, w, buf, false, true);
  }
}
