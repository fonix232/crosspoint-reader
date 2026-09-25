#include "BookOrbitSettingsActivity.h"

#include <BookOrbitCredentialStore.h>
#include <BookOrbitDownloadQueue.h>
#include <BookOrbitSyncState.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <ctime>
#include <memory>

#include "BookOrbitAuthActivity.h"
#include "BookOrbitBrowseActivity.h"
#include "BookOrbitQueueActivity.h"
#include "BookOrbitSyncActivity.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "bookorbit/BookOrbitAgent.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
enum Row : int {
  SERVER = 0,
  USERNAME,
  PASSWORD,
  INSECURE_TLS,
  SYNC_PROGRESS,
  SYNC_INTERVAL,
  TEST,
  SYNC_NOW,
  QUEUE,
  BROWSE,
  LIBRARIES,
  SIGN_OUT
};

const StrId menuNames[BookOrbitSettingsActivity::MENU_ITEMS] = {StrId::STR_BOOKORBIT_SERVER_URL,
                                                                StrId::STR_USERNAME,
                                                                StrId::STR_PASSWORD,
                                                                StrId::STR_BOOKORBIT_INSECURE_TLS,
                                                                StrId::STR_BOOKORBIT_SYNC_PROGRESS,
                                                                StrId::STR_BOOKORBIT_SYNC_INTERVAL,
                                                                StrId::STR_BOOKORBIT_TEST_CONNECTION,
                                                                StrId::STR_BOOKORBIT_SYNC_NOW,
                                                                StrId::STR_BOOKORBIT_QUEUE_ROW,
                                                                StrId::STR_BOOKORBIT_BROWSE,
                                                                StrId::STR_BOOKORBIT_LIBRARIES,
                                                                StrId::STR_BOOKORBIT_SIGN_OUT};

// Value steps for the progress sync interval (minutes; 0 = off).
constexpr uint8_t INTERVAL_STEPS[] = {0, 15, 30, 60, 120};

template <size_t N>
uint8_t nextStep(const uint8_t (&steps)[N], const uint8_t current) {
  for (size_t i = 0; i < N; i++) {
    if (steps[i] == current) return steps[(i + 1) % N];
  }
  return steps[0];
}

// "Last sync" value: a local date/time when the clock was valid at the time,
// else the plain never/failed state.
void formatLastSync(std::string& out) {
  bookorbit::SyncState state;
  if (!bookorbit::loadSyncState(BOOKORBIT_STORE.getAccountKey(), state) || !state.lastOk) {
    out = tr(STR_BOOKORBIT_NEVER);
    return;
  }
  if (state.lastSuccessAt == 0) {  // synced, but the clock was unset at the time
    out = tr(STR_BOOKORBIT_SYNC_DONE);
    return;
  }
  const time_t when = static_cast<time_t>(state.lastSuccessAt);
  struct tm local;
  localtime_r(&when, &local);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
  out = buf;
}
}  // namespace

BookOrbitSettingsActivity::BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("BookOrbitSettings", renderer, mappedInput) {
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

int BookOrbitSettingsActivity::listCount() const { return MENU_ITEMS; }

const char* BookOrbitSettingsActivity::headerTitle() const { return tr(STR_BOOKORBIT); }

void BookOrbitSettingsActivity::editServerUrl() {
  const std::string current = BOOKORBIT_STORE.getServerUrl();
  const std::string prefill = current.empty() ? "https://" : current;
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_SERVER_URL),
                                                                 prefill, 160, InputType::Url),
                         [](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const auto& kb = std::get<KeyboardResult>(result.data);
                           const std::string url = (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                           BOOKORBIT_STORE.setServerUrl(url);
                           BOOKORBIT_STORE.saveToFile();
                         });
}

void BookOrbitSettingsActivity::editUsername() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_USERNAME),
                                                                 BOOKORBIT_STORE.getUsername(), 64, InputType::Text),
                         [](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const auto& kb = std::get<KeyboardResult>(result.data);
                           BOOKORBIT_STORE.setCredentials(kb.text, BOOKORBIT_STORE.getPassword());
                           BOOKORBIT_STORE.saveToFile();
                         });
}

void BookOrbitSettingsActivity::editPassword() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_PASSWORD),
                                                                 BOOKORBIT_STORE.getPassword(), 64, InputType::Text),
                         [](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const auto& kb = std::get<KeyboardResult>(result.data);
                           BOOKORBIT_STORE.setCredentials(BOOKORBIT_STORE.getUsername(), kb.text);
                           BOOKORBIT_STORE.saveToFile();
                         });
}

void BookOrbitSettingsActivity::testConnection() {
  if (!BOOKORBIT_STORE.isConfigured()) return;
  startActivityForResult(std::make_unique<BookOrbitAuthActivity>(renderer, mappedInput), [](const ActivityResult&) {});
}

void BookOrbitSettingsActivity::openQueue() {
  if (!BOOKORBIT_STORE.isConfigured()) return;
  auto activity = makeUniqueNoThrow<BookOrbitQueueActivity>(renderer, mappedInput);
  if (!activity) return;
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
}

void BookOrbitSettingsActivity::syncNow() {
  if (!BOOKORBIT_STORE.isConfigured()) return;
  // The session reboots on exit (heap defrag after Wi-Fi) and lands back on
  // this screen, so it replaces it rather than stacking on it.
  activityManager.replaceActivity(
      std::make_unique<BookOrbitSyncActivity>(renderer, mappedInput, BookOrbitSyncActivity::ReturnTo::Settings));
}

void BookOrbitSettingsActivity::promptSignOut() {
  if (!BOOKORBIT_STORE.isConfigured()) return;
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_SIGN_OUT),
                                                              tr(STR_BOOKORBIT_SIGN_OUT_CONFIRM));
  if (!confirmation) {
    LOG_ERR("BO", "OOM: sign-out confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation), [](const ActivityResult& result) {
    if (result.isCancelled) return;
    // The account namespace (cache, downloads, acknowledgements) stays on the
    // card, detached from whatever account is configured next.
    BOOKORBIT_STORE.clearCredentials();
  });
}

bool BookOrbitSettingsActivity::handleCustomInput() {
  auto& agent = BOOKORBIT_AGENT;
  if (!agent.available()) return false;
  // The Sync now row shows the background job's state; repaint as it changes.
  const uint8_t state = agent.busy() ? 1 : 0;
  if (state != lastAgentState) {
    lastAgentState = state;
    requestUpdate();
  }
  return false;
}

void BookOrbitSettingsActivity::cycleSyncInterval() {
  BOOKORBIT_STORE.setProgressSyncMinutes(nextStep(INTERVAL_STEPS, BOOKORBIT_STORE.getProgressSyncMinutes()));
  BOOKORBIT_STORE.saveToFile();
  requestUpdate();
}

void BookOrbitSettingsActivity::activateIndex(const int index) {
  app.clearTapFlash();
  switch (index) {
    case SERVER:
      editServerUrl();
      break;
    case USERNAME:
      editUsername();
      break;
    case PASSWORD:
      editPassword();
      break;
    case INSECURE_TLS:
      BOOKORBIT_STORE.setInsecureTls(!BOOKORBIT_STORE.getInsecureTls());
      BOOKORBIT_STORE.saveToFile();
      requestUpdate();
      break;
    case SYNC_PROGRESS:
      BOOKORBIT_STORE.setSyncProgress(!BOOKORBIT_STORE.getSyncProgress());
      BOOKORBIT_STORE.saveToFile();
      requestUpdate();
      break;
    case SYNC_INTERVAL:
      cycleSyncInterval();
      break;
    case TEST:
      testConnection();
      break;
    case SYNC_NOW:
      syncNow();
      break;
    case QUEUE:
      openQueue();
      break;
    case BROWSE: {
      auto browse = makeUniqueNoThrow<BookOrbitBrowseActivity>(renderer, mappedInput);
      if (browse) startActivityForResult(std::move(browse), [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case LIBRARIES: {
      if (!BOOKORBIT_STORE.isConfigured()) break;
      auto picker = makeUniqueNoThrow<BookOrbitBrowseActivity>(renderer, mappedInput, true);
      if (picker) startActivityForResult(std::move(picker), [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case SIGN_OUT:
      promptSignOut();
      break;
    default:
      break;
  }
}

void BookOrbitSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const bool configured = BOOKORBIT_STORE.isConfigured();
  for (int i = 0; i < MENU_ITEMS; i++) {
    switch (i) {
      case SERVER:
        rowValues_[i] = BOOKORBIT_STORE.getServerUrl().empty() ? tr(STR_NOT_SET) : BOOKORBIT_STORE.getOrigin();
        break;
      case USERNAME:
        rowValues_[i] = BOOKORBIT_STORE.getUsername().empty() ? tr(STR_NOT_SET) : BOOKORBIT_STORE.getUsername();
        break;
      case PASSWORD:
        rowValues_[i] = BOOKORBIT_STORE.getPassword().empty() ? tr(STR_NOT_SET) : "******";
        break;
      case INSECURE_TLS:
        rowValues_[i] = BOOKORBIT_STORE.getInsecureTls() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        break;
      case SYNC_PROGRESS:
        rowValues_[i] = BOOKORBIT_STORE.getSyncProgress() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        break;
      case SYNC_INTERVAL: {
        char buf[24];
        const uint8_t minutes = BOOKORBIT_STORE.getProgressSyncMinutes();
        if (!BookOrbitAgent::supported()) {
          rowValues_[i] = tr(STR_BOOKORBIT_UNSUPPORTED);
        } else if (minutes == 0) {
          rowValues_[i] = tr(STR_STATE_OFF);
        } else {
          snprintf(buf, sizeof(buf), tr(STR_SLEEP_TIMER_VALUE_FORMAT), static_cast<unsigned>(minutes));
          rowValues_[i] = buf;
        }
        break;
      }
      case SYNC_NOW:
        if (configured) {
          formatLastSync(rowValues_[i]);
        } else {
          rowValues_[i] = std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
        }
        break;
      case LIBRARIES: {
        const auto& selection = BOOKORBIT_STORE.getLibraries();
        char count[32];
        snprintf(count, sizeof(count), tr(STR_BOOKORBIT_LIBRARIES_SELECTED), selection.count);
        rowValues_[i] = selection.all ? tr(STR_BOOKORBIT_ALL_LIBRARIES) : count;
        break;
      }
      case QUEUE: {
        bookorbit::DownloadQueue queue;
        queue.load(BOOKORBIT_STORE.getAccountKey());
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(queue.items().size()));
        rowValues_[i] = configured ? buf : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
        break;
      }
      default:
        rowValues_[i] = configured ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
        break;
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEMS);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
