#!/usr/bin/env python3
"""Exercise production sleep GPIO handling across reset and PWM backends."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'freeink-sdk/libs/hardware/FrontlightManager/src/FrontlightManager.cpp').read_text()
start = source.index('void FrontlightManager::park()')
end = source.index('void FrontlightManager::setBrightness(', start)
production = source[start:end]
# Every sleep entry, including early rejected wakes, must park the frontlight.
power = (root / 'lib/hal/HalPowerManager.cpp').read_text()
sleep = power[power.index('void HalPowerManager::startDeepSleep('):]
assert sleep.index('Frontlight.prepareForDeepSleep();') < sleep.index('logSerial.end();')
begin = source[source.index('void FrontlightManager::begin()'):start]
assert begin.index('releaseOnWake();') < begin.index('attachChannel(')
fixture = r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
using gpio_num_t = int;
constexpr int LOW = 0, HIGH = 1, OUTPUT = 1;
constexpr int LEDC_LOW_SPEED_MODE = 0, LEDC_CHANNEL_0 = 0, LEDC_CHANNEL_1 = 1;
struct Pin { bool held = false; int level = 1; bool output = false; } pins[16];
int detached = 0, stopped = 0;
void gpio_hold_dis(int p) { assert(p >= 0); pins[p].held = false; }
void gpio_hold_en(int p) { assert(pins[p].output); pins[p].held = true; }
void pinMode(int p, int) { assert(!pins[p].held); pins[p].output = true; }
void digitalWrite(int p, int level) { assert(!pins[p].held); pins[p].level = level; }
void ledcDetach(int) { ++detached; }
void ledcDetachPin(int) { ++detached; }
void ledc_stop(int, int, int) { ++stopped; }
namespace BoardConfig {
struct Profile { struct { int8_t gpio = 8, gpioWarm = 9; bool activeHigh = true, viaPm1Pwm = false; } frontlight; } ACTIVE;
}
struct FrontlightManager {
  bool _begun = false;
  bool keepAlive = true;
  int brightness = 60;
  void setBrightness(int value) { brightness = value; }
  void updateLsKeepAlive(bool value) { keepAlive = value; }
  void park();
  void releaseOnWake();
};
'''
tests = r'''
int main() {
#if FREEINK_CAP_FRONTLIGHT
  for (bool begun : {false, true}) {
    for (bool activeHigh : {false, true}) {
      auto& fl = BoardConfig::ACTIVE.frontlight;
      fl.activeHigh = activeHigh;
      detached = stopped = 0;
      FrontlightManager light;
      light._begun = begun;
      // Simulate retained holds from an earlier sleep cycle.
      pins[8].held = pins[9].held = true;
      light.park();
      for (int p : {8, 9}) {
        assert(pins[p].held && pins[p].output);
        assert(pins[p].level == (activeHigh ? LOW : HIGH));
      }
      if (begun) assert(light.brightness == 0);
#ifdef FREEINK_FRONTLIGHT_LS
      assert(stopped == (begun ? 2 : 0));
      if (begun) assert(!light.keepAlive);
#else
      assert(detached == (begun ? 2 : 0));
#endif
      // Wake creates a fresh object: releasing holds must not depend on DRAM flags.
      FrontlightManager wake;
      wake.releaseOnWake();
      assert(!pins[8].held && !pins[9].held);
    }
  }
  BoardConfig::ACTIVE.frontlight.gpio = -1;
  BoardConfig::ACTIVE.frontlight.gpioWarm = -1;
#endif
  FrontlightManager absent;
  absent.park();
  absent.releaseOnWake();
}
'''
with tempfile.TemporaryDirectory(prefix='sleep-frontlight-') as directory:
    work = Path(directory)
    cpp = work / 'test.cpp'
    cpp.write_text(fixture + production + tests)
    for flags in [ ['-DFREEINK_CAP_FRONTLIGHT=0'],
                   ['-DFREEINK_CAP_FRONTLIGHT=1', '-DESP_ARDUINO_VERSION_MAJOR=2'],
                   ['-DFREEINK_CAP_FRONTLIGHT=1', '-DESP_ARDUINO_VERSION_MAJOR=3'],
                   ['-DFREEINK_CAP_FRONTLIGHT=1', '-DFREEINK_FRONTLIGHT_LS'] ]:
        subprocess.run(['c++', '-std=c++17', *flags, str(cpp), '-o', str(work / 'test')], check=True)
        subprocess.run([str(work / 'test')], check=True)
print('PASS: normal and rejected-wake sleep, both PWM backends, polarity, retained holds, no frontlight')
