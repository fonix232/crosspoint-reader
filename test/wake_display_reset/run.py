#!/usr/bin/env python3
"""Verify the production HAL initialization policy across boards and wake causes."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'lib/hal/HalDisplay.cpp').read_text()
body = source[source.index('void HalDisplay::begin('):source.index('void HalDisplay::clearScreen(')]
fixture = r'''
#include <cassert>
#include <cstdint>
#include <vector>
namespace BoardConfig { bool pro=false; bool isX4Pro() { return pro; } }
struct HalGPIO {
  enum class WakeupReason { PowerButton, AfterFlash, Other, AfterUSBPower };
  WakeupReason reason=WakeupReason::PowerButton;
  bool x3=false;
  bool deviceIsX3() const { return x3; }
  WakeupReason getWakeupReason() const { return reason; }
} gpio;
struct EInkDisplay {
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  std::vector<int> events;
  void setDisplayX3() { events.push_back(0); }
  void begin() { events.push_back(1); }
  void clearScreen(uint8_t color) { assert(color==255);events.push_back(2); }
  void displayBuffer(RefreshMode mode,bool off) { assert(mode==FULL_REFRESH && off);events.push_back(3); }
  void skipInitialResync() { events.push_back(4); }
  void requestResync() { events.push_back(5); }
};
struct HalDisplay { EInkDisplay einkDisplay;void begin(bool); };
'''
tests = r'''
int main() {
  for(bool seamless:{false,true}) for(auto reason:{HalGPIO::WakeupReason::PowerButton,
      HalGPIO::WakeupReason::AfterFlash,HalGPIO::WakeupReason::Other,HalGPIO::WakeupReason::AfterUSBPower}) {
    gpio.reason=reason;BoardConfig::pro=true;gpio.x3=false;
    HalDisplay pro;pro.begin(seamless);
    assert(pro.einkDisplay.events==std::vector<int>({1,2,3}));
    BoardConfig::pro=false;
    HalDisplay paper;paper.begin(seamless);
    assert(paper.einkDisplay.events.front()==1);
    for(int event:paper.einkDisplay.events) assert(event!=2 && event!=3);
    if(seamless) assert(paper.einkDisplay.events==std::vector<int>({1,4}));
    gpio.x3=true;
    HalDisplay x3;x3.begin(true);
    assert(x3.einkDisplay.events==std::vector<int>({0,1,4}));
  }
}
'''
with tempfile.TemporaryDirectory(prefix='wake-display-') as directory:
    root = Path(directory)
    (root / 'test.cpp').write_text(fixture + body + tests)
    subprocess.run(['c++', '-std=c++17', str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
print('Wake initialization: X4 Pro always clears fully; Paper no-wipe and X3 resume policies preserved')
