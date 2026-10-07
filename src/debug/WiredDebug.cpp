#include "WiredDebug.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include "WiredDebugProtocol.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"

namespace wired_debug {
namespace {
class SerialTimeout {
 public:
  SerialTimeout() {
#if LOG_SERIAL_HAS_TX_TIMEOUT
    logSerial.setTxTimeoutMs(250);
#endif
  }
  ~SerialTimeout() {
#if LOG_SERIAL_HAS_TX_TIMEOUT
    logSerial.setTxTimeoutMs(1);
#endif
  }
};

void error(uint32_t id, const char* reason) { logSerial.printf("\nDBG:%u ERROR %s\n", id, reason); }

bool point(GfxRenderer& renderer, uint32_t x, uint32_t y, DebugInput::Point& result) {
  if (x >= static_cast<uint32_t>(renderer.getScreenWidth()) || y >= static_cast<uint32_t>(renderer.getScreenHeight()))
    return false;
  const int width = renderer.getDisplayWidth(), height = renderer.getDisplayHeight();
  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait:
      result = {static_cast<int16_t>(y), static_cast<int16_t>(height - 1 - x)};
      break;
    case GfxRenderer::PortraitInverted:
      result = {static_cast<int16_t>(width - 1 - y), static_cast<int16_t>(x)};
      break;
    case GfxRenderer::LandscapeClockwise:
      result = {static_cast<int16_t>(width - 1 - x), static_cast<int16_t>(height - 1 - y)};
      break;
    default:
      result = {static_cast<int16_t>(x), static_cast<int16_t>(y)};
      break;
  }
  return true;
}

void screenshot(uint32_t id, GfxRenderer& renderer, HalDisplay& display) {
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock() || !activityManager.debugRenderIdle()) {
    error(id, "BUSY_RENDER");
    return;
  }
  const auto* buffer = display.getFrameBuffer();
  if (!buffer) {
    error(id, "NO_FRAMEBUFFER");
    return;
  }
  const uint32_t size = display.getBufferSize();
  logSerial.printf("\nDBG:%u SCREENSHOT %u %u %u %u %u\n", id, display.getDisplayWidth(), display.getDisplayHeight(),
                   display.getDisplayWidthBytes(), static_cast<unsigned>(renderer.getOrientation()), size);
  const auto started = millis();
  uint32_t crc = 0xffffffffu;
  // One checked write per framed line; logs may appear between lines. No framebuffer copy.
  for (uint32_t offset = 0; offset < size; offset += 64) {
    if (!logSerial || millis() - started > 30000) {
      error(id, "TRANSFER_TIMEOUT");
      return;
    }
    char line[176];
    const size_t count = size - offset < 64 ? size - offset : 64;
    int length = snprintf(line, sizeof(line), "\nDBG:%u DATA %u ", id, offset);
    static constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
      line[length++] = hex[buffer[offset + i] >> 4];
      line[length++] = hex[buffer[offset + i] & 15];
    }
    line[length++] = '\n';
    if (logSerial.write(reinterpret_cast<const uint8_t*>(line), length) != static_cast<size_t>(length)) {
      error(id, "SHORT_WRITE");
      return;
    }
    crc = crc32(crc, buffer + offset, count);
    delay(1);
  }
  logSerial.printf("\nDBG:%u END %08x\n", id, crc ^ 0xffffffffu);
}
}  // namespace

bool handleCommand(const char* command, GfxRenderer& renderer, HalDisplay& display, bool exclusiveStorage) {
  if (strncmp(command, "CMD:DEBUG", 9) || (command[9] && command[9] != ' ')) return false;
  SerialTimeout timeout;
  Request request;
  if (!parse(command, request)) {
    error(request.id, "ARGUMENTS");
    return true;
  }
  auto& input = gpio.debugInput();
  if (request.operation == Operation::Reset) {
    input.reset();
    logSerial.printf("\nDBG:%u OK RESET\n", request.id);
    return true;
  }
  if (request.operation == Operation::Info) {
    RenderLock lock;
    logSerial.printf("\nDBG:%u INFO 1 %d %d %u %u %u %u %lu\n", request.id, renderer.getScreenWidth(),
                     renderer.getScreenHeight(), static_cast<unsigned>(renderer.getOrientation()), gpio.hasTouch(),
                     gpio.hasHomeKey(), exclusiveStorage, millis());
    return true;
  }
  if (request.operation == Operation::Power) {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock() || !activityManager.debugRenderIdle()) {
      error(request.id, "BUSY_RENDER");
      return true;
    }
    const auto d = powerManager.diagnostics();
    logSerial.printf("\nDBG:%u POWER 2 %d %d %d %d %d %d %d %d %d %d %d %lu\n", request.id, d.millivolts,
                     d.estimatedPercent, d.displayedPercent, d.cpuMhz, d.wifiMode, d.wifiConnected, d.usbConnected,
                     d.charging, d.epdPower, d.epdOutputEnable, d.usbMillivolts, millis());
    return true;
  }
  if (exclusiveStorage) {
    input.reset();
    error(request.id, "USB_STORAGE_ACTIVE");
    return true;
  }
  if (request.operation == Operation::Screenshot) {
    if (input.active())
      error(request.id, "INPUT_ACTIVE");
    else
      screenshot(request.id, renderer, display);
    return true;
  }
  if (request.operation == Operation::Button) {
    if (request.button == DebugInput::HOME && !gpio.hasHomeKey()) {
      error(request.id, "NO_HOME_KEY");
      return true;
    }
    if (!input.button(request.button, request.action != Action::Up, request.duration)) {
      error(request.id, "BUTTON_STATE");
      return true;
    }
  } else {
    if (!gpio.hasTouch()) {
      error(request.id, "NO_TOUCH");
      return true;
    }
    RenderLock lock;
    DebugInput::Point start, end;
    gpio.setDebugGeometry(renderer.getDisplayWidth(), renderer.getDisplayHeight());
    if (request.action != Action::Up && !point(renderer, request.x, request.y, start)) {
      error(request.id, "COORDINATES");
      return true;
    }
    if (request.action == Action::Swipe && !point(renderer, request.endX, request.endY, end)) {
      error(request.id, "COORDINATES");
      return true;
    }
    bool accepted = false;
    switch (request.action) {
      case Action::Down:
        accepted = input.touchDown(start);
        break;
      case Action::Move:
        accepted = input.touchMove(start);
        break;
      case Action::Up:
        accepted = input.touchUp();
        break;
      case Action::Tap:
        accepted = input.touchDown(start, request.duration, start);
        break;
      case Action::Swipe:
        accepted = input.touchDown(start, request.duration, end);
        break;
      default:
        break;
    }
    if (!accepted) {
      error(request.id, "TOUCH_STATE");
      return true;
    }
  }
  logSerial.printf("\nDBG:%u OK\n", request.id);
  return true;
}
}  // namespace wired_debug
