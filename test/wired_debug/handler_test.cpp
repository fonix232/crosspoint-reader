#include <cassert>

#include "Fakes.h"
#include "WiredDebug.h"

int main() {
  GfxRenderer renderer;
  HalDisplay display;
  auto command = [&](const char* text, bool exclusive = false) {
    logSerial.output.clear();
    assert(wired_debug::handleCommand(text, renderer, display, exclusive));
    assert(logSerial.timeout == 1);
    return logSerial.output;
  };
  assert(!wired_debug::handleCommand("CMD:DEBUGGING", renderer, display, false));
  assert(!wired_debug::handleCommand("CMD:FW_INFO", renderer, display, false));
  assert(command("CMD:DEBUG 1 INFO").find("DBG:1 INFO 1 480 800 0 1 1 0 1000") != std::string::npos);
  assert(command("CMD:DEBUG 90 POWER").find("POWER 2 4010 80 86 80 0 0 1 1 0 0 420 1000") != std::string::npos);
  assert(command("CMD:DEBUG 91 POWER extra").find("ARGUMENTS") != std::string::npos);
  activityManager.idle = false;
  assert(command("CMD:DEBUG 92 POWER").find("BUSY_RENDER") != std::string::npos);
  activityManager.idle = true;
  assert(command("CMD:DEBUG 2 BUTTON CONFIRM CLICK 120") == "\nDBG:2 OK\n");
  gpio.input.update(fakeNow);
  assert(gpio.input.pressed == 2);
  assert(command("CMD:DEBUG 3 SCREENSHOT").find("INPUT_ACTIVE") != std::string::npos);
  assert(command("CMD:DEBUG 4 RESET") == "\nDBG:4 OK RESET\n");
  assert(!gpio.input.active());
  assert(command("CMD:DEBUG 5 BUTTON CONFIRM CLICK 120", true).find("USB_STORAGE_ACTIVE") != std::string::npos);
  assert(!gpio.input.active());
  assert(command("CMD:DEBUG 6 TOUCH DOWN 480 0").find("COORDINATES") != std::string::npos);
  assert(command("CMD:DEBUG 7 TOUCH MOVE 0 0").find("TOUCH_STATE") != std::string::npos);
  gpio.touch = false;
  assert(command("CMD:DEBUG 8 TOUCH TAP 1 2 120").find("NO_TOUCH") != std::string::npos);
  gpio.touch = true;
  gpio.home = false;
  assert(command("CMD:DEBUG 9 BUTTON HOME CLICK 120").find("NO_HOME_KEY") != std::string::npos);
  assert(command("CMD:DEBUG 10 TOUCH DOWN 1 2") == "\nDBG:10 OK\n");
  assert(gpio.input.position.x == 2 && gpio.input.position.y == 478);
  command("CMD:DEBUG 11 RESET");
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.orientation = static_cast<GfxRenderer::Orientation>(orientation);
    command("CMD:DEBUG 12 TOUCH TAP 0 0 120");
    static constexpr int expected[4][2] = {{0, 479}, {799, 479}, {799, 0}, {0, 0}};
    assert(gpio.input.position.x == expected[orientation][0] && gpio.input.position.y == expected[orientation][1]);
    command("CMD:DEBUG 13 RESET");
  }
  renderer.orientation = GfxRenderer::Portrait;
  activityManager.idle = false;
  assert(command("CMD:DEBUG 14 SCREENSHOT").find("BUSY_RENDER") != std::string::npos);
  activityManager.idle = true;
  renderAvailable = false;
  assert(command("CMD:DEBUG 15 SCREENSHOT").find("BUSY_RENDER") != std::string::npos);
  renderAvailable = true;
  const auto capture = command("CMD:DEBUG 16 SCREENSHOT");
  assert(capture.find("DBG:16 SCREENSHOT 8 2 1 0 2") != std::string::npos);
  assert(capture.find("DBG:16 DATA 0 8040\n") != std::string::npos);
  assert(capture.find("DBG:16 END") != std::string::npos);
  logSerial.shortWrite = true;
  const auto failed = command("CMD:DEBUG 17 SCREENSHOT");
  assert(failed.find("SHORT_WRITE") != std::string::npos && failed.find(" END ") == std::string::npos);
  assert(command("CMD:DEBUG 18 BUTTON POWER CLICK 99999").find("ARGUMENTS") != std::string::npos);
}
