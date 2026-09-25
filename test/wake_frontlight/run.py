#!/usr/bin/env python3
"""Exercise the production wake gate before any frontlight restoration."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/main.cpp').read_text()
start = source.index('  SETTINGS.loadFromFile();', source.index('void setup('))
end = source.index('  HalDoubleTap::begin(', start)
gate = source[start:end]
assert end < source.index('  Frontlight.begin(', start)
fixture = r'''
#include <cassert>
#include <initializer_list>
#define LOG_DBG(...) ((void)0)
struct HalGPIO { enum class WakeupReason { PowerButton, AfterUSBPower, AfterFlash, Other }; } gpio;
struct CrossPointSettings { enum class SHORT_PWRBTN { SLEEP, IGNORE, OTHER }; };
struct {
  CrossPointSettings::SHORT_PWRBTN shortPwrBtn;
  bool loaded = false;
  void loadFromFile() { loaded = true; }
} SETTINGS;
struct {
  bool prepared = false;
  void prepareForDeepSleep() { assert(SETTINGS.loaded); prepared = true; }
} Storage;
struct {
  bool slept = false;
  void startDeepSleep(HalGPIO&) { assert(Storage.prepared); slept = true; }
} powerManager;
bool lightRestored = false;
void boot(HalGPIO::WakeupReason wakeupReason, bool wakeHoldVerified) {
'''
tests = r'''
  lightRestored = true;
}
int main() {
  for (auto reason : {HalGPIO::WakeupReason::PowerButton, HalGPIO::WakeupReason::AfterUSBPower,
                      HalGPIO::WakeupReason::AfterFlash, HalGPIO::WakeupReason::Other}) {
    for (bool held : {false, true}) {
      for (auto action : {CrossPointSettings::SHORT_PWRBTN::SLEEP, CrossPointSettings::SHORT_PWRBTN::IGNORE,
                          CrossPointSettings::SHORT_PWRBTN::OTHER}) {
        SETTINGS.shortPwrBtn = action;
        SETTINGS.loaded = Storage.prepared = powerManager.slept = lightRestored = false;
        boot(reason, held);
        const bool rejected = reason == HalGPIO::WakeupReason::PowerButton && !held &&
                              action != CrossPointSettings::SHORT_PWRBTN::SLEEP;
        assert(powerManager.slept == rejected);
        assert(Storage.prepared == rejected);
        assert(lightRestored == !rejected);
      }
    }
  }
}
'''
with tempfile.TemporaryDirectory(prefix='wake-frontlight-') as directory:
    work = Path(directory)
    cpp = work / 'test.cpp'
    cpp.write_text(fixture + gate + tests)
    subprocess.run(['c++', '-std=c++17', str(cpp), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('PASS: rejected wakes stay dark; held wakes, click-to-wake and other boot reasons proceed')
