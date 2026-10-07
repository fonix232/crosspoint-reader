#include "PowerLog.h"

#include <BoardConfig.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_system.h>

#include <ctime>

#include "activities/RenderLock.h"

namespace power_log {
namespace {
constexpr const char* PATH = "/.crosspoint/power-v2.csv";
constexpr unsigned long INTERVAL_MS = 60000;
constexpr size_t MAX_BYTES = 16 * 1024 * 1024;
unsigned long lastSample = 0;
unsigned long lastCheck = 0;
int lastUsb = -1;
uint32_t session = 0;
// Main-task-only scratch space; avoids adding a large buffer to the task stack.
char line[224];
}  // namespace

void record(const char* event, bool reading) {
  if (!BoardConfig::isM5PaperS3() || !Storage.ready()) return;
  if (!session) session = esp_random();
  const auto d = powerManager.diagnostics();
  time_t utc = time(nullptr);
  if (utc < 1704067200 && !halClock.utcNow(utc)) utc = 0;
  if (utc < 1704067200) utc = 0;
  const int length =
      snprintf(line, sizeof(line), "%08lx,%lld,%lu,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%llu,%lu\n",
               static_cast<unsigned long>(session), static_cast<long long>(utc), millis(), event, d.millivolts,
               d.estimatedPercent, d.displayedPercent, d.usbConnected, d.usbMillivolts, d.charging, d.cpuMhz,
               d.wifiMode, d.wifiConnected, d.epdPower, d.epdOutputEnable, reading,
               static_cast<int>(esp_reset_reason()), static_cast<unsigned long long>(powerManager.idleSleepUs),
               static_cast<unsigned long>(powerManager.idleSleepCount));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(line)) return;
  if (!Storage.ensureDirectoryExists("/.crosspoint")) return;
  // Open/close once per minute or power event; no retained file across MSC/sleep.
  auto file = Storage.open(PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file || file.size() >= MAX_BYTES) {
    LOG_ERR("PWRLOG", "Cannot append power log (unavailable or 16 MiB limit)");
    return;
  }
  if (file.size() == 0) {
    file.print(
        "session,utc,uptime_ms,event,mv,raw_pct,ui_pct,usb,usb_mv,charging,cpu_mhz,wifi_mode,wifi_connected,epd_power,"
        "epd_oe,reading,reset_reason,idle_sleep_us,idle_sleep_count\n");
  }
  if (file.write(reinterpret_cast<const uint8_t*>(line), length) != static_cast<size_t>(length)) {
    LOG_ERR("PWRLOG", "Power log write failed");
  }
  file.flush();
}

void poll(bool reading) {
  if (!BoardConfig::isM5PaperS3()) return;
  const auto now = millis();
  if (now - lastCheck < 250) return;
  lastCheck = now;
  const int usb = gpio.isUsbConnected();
  const bool changed = lastUsb >= 0 && usb != lastUsb;
  if (!changed && now - lastSample < INTERVAL_MS) {
    if (lastUsb < 0) lastUsb = usb;
    return;
  }
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return;
  powerManager.getBatteryPercentage();  // Keep the cached estimate advancing during an unchanged page.
  record(changed ? (usb ? "usb_plug" : "usb_unplug") : "sample", reading);
  lastUsb = usb;
  lastSample = now;
}

void exportLog() {
  logSerial.println("POWERLOG_BEGIN");
  Storage.readFileToStream(PATH, logSerial, 192);
  logSerial.println("POWERLOG_END");
}
}  // namespace power_log
