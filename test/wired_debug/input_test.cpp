#include <cassert>
#include <string>

#include "DebugInput.h"
#include "WiredDebugProtocol.h"

int main() {
  DebugInput input;
  assert(sizeof(input) < 160);
  assert(input.button(1, true, 120));
  assert(!input.button(1, true));
  input.update(100);
  assert(input.buttons == 2 && input.pressed == 2 && !input.released);
  input.update(219);
  assert(input.buttons == 2 && !input.pressed && input.held(219) == 119);
  input.update(220);
  assert(!input.buttons && input.released == 2 && input.held(220) == 120);
  input.update(221);
  assert(!input.released);
  assert(!input.button(9, true));
  assert(!input.button(1, false));
  assert(input.button(6, true));
  input.update(300);
  assert(input.button(6, false));
  input.update(600);
  assert(input.released == 64 && input.lastPowerHeld == 300);
  assert(input.button(1, true));
  input.update(700);
  input.update(10700);
  assert(!input.buttons && !input.released);  // watchdog cancellation never clicks
  assert(input.button(7, true, 900));
  input.update(11000);
  input.update(11700);
  assert(input.homeLong);
  input.update(11900);
  assert(!input.homeLong && !input.homeTap);
  assert(input.button(7, true, 100));
  input.update(12000);
  input.update(12100);
  assert(input.homeTap);

  input.reset();
  assert(input.touchDown({100, 200}, 120, {100, 200}));
  assert(!input.touchDown({100, 200}));
  input.update(0xfffffff0u);
  assert(input.down && input.touch && !input.tap());
  input.update(0x68);
  assert(input.up && input.tap() && !input.swipe() && input.touchHeld == 120);
  input.update(0x69);
  assert(!input.tap());

  assert(input.touchDown({100, 200}, 300, {400, 200}));
  input.update(1000);
  input.update(1150);
  assert(input.position.x == 250 && input.moved && !input.tap());
  input.update(1300);
  assert(input.swipe() && !input.tap() && input.position.x == 400);

  assert(input.touchDown({400, 500}, 300, {100, 200}));
  input.update(2000);
  input.update(2150);
  assert(input.position.x == 250 && input.position.y == 350);
  input.update(2300);
  assert(input.swipe() && input.position.x == 100 && input.position.y == 200);

  assert(input.touchDown({100, 200}));
  input.update(2000);
  input.update(2500);
  assert(input.longPress && input.longFired);
  input.suppressed = true;
  assert(input.touchUp());
  input.update(2600);
  assert(!input.tap() && !input.up);

  assert(input.touchDown({100, 200}));
  input.update(3000);
  assert(input.touchMove({130, 200}));
  input.update(3100);
  assert(input.moved && !input.tapMoved);
  assert(input.touchUp());
  input.update(3200);
  assert(input.tap() && !input.swipe());
  assert(!input.touchMove({1, 1}));
  assert(!input.touchUp());
  assert(input.touchDown({1, 1}));
  input.update(4000);
  input.update(14000);
  assert(!input.touch && input.up && !input.tap() && !input.swipe());
  assert(input.touchDown({1, 1}));
  input.update(14500);
  input.reset();
  input.update(14501);
  assert(input.up && !input.touch && !input.tap() && !input.swipe());
  assert(input.button(1, true));
  assert(input.button(2, true));
  input.update(15000);
  assert(input.pressed == 6);
  input.reset();
  input.update(15001);
  assert(!input.active() && !input.released);

  using namespace wired_debug;
  for (const auto* command : {"CMD:DEBUG 1 INFO", "CMD:DEBUG 2147483647 RESET", "CMD:DEBUG 2 SCREENSHOT",
                              "CMD:DEBUG 3 BUTTON POWER CLICK 5000", "CMD:DEBUG 4 BUTTON HOME UP",
                              "CMD:DEBUG 5 TOUCH TAP 0 32767 120", "CMD:DEBUG 6 TOUCH SWIPE 1 2 3 4 700"}) {
    Request request;
    assert(parse(command, request));
  }
  for (const auto* command : {"CMD:DEBUG 0 INFO", "CMD:DEBUG -1 INFO", "CMD:DEBUG 2147483648 INFO",
                              "CMD:DEBUG 1 INFO extra", "CMD:DEBUG 1 BUTTON BAD DOWN", "CMD:DEBUG 1 TOUCH UP 1",
                              "CMD:DEBUG 1 TOUCH TAP -1 0 120", "CMD:DEBUG 1 TOUCH SWIPE 1 2 3 4 701",
                              "CMD:DEBUG 1 BUTTON POWER CLICK 0", "CMD:DEBUG 1 BUTTON POWER CLICK 5001",
                              "CMD:DEBUG 1 TOUCH DOWN 1", "CMD:DEBUG 1 TOUCH TAP 1 2 99999999999999999999"}) {
    Request request;
    assert(!parse(command, request));
  }
  const uint8_t known[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  assert((crc32(0xffffffff, known, sizeof(known)) ^ 0xffffffff) == 0xcbf43926);
}
