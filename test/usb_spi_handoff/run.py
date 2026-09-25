#!/usr/bin/env python3
"""Exercise the real SPI handoff with ordered filesystem/card lifecycle spies."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'freeink-sdk/libs/hardware/SDCardManager/src/SDCardManager.cpp').read_text()
start = s.index('FsBlockDeviceInterface* SDCardManager::detachFilesystemForRawAccess()', s.index('#if USE_BLOCK_DEVICE_INTERFACE || HAS_SDIO_CLASS'))
end = s.index('\n#else', start)
fixture = r'''
#include <cassert>
#include <cstdint>
#include <vector>
constexpr int O_RDONLY=0;
std::vector<int> events;
bool openOk=true, syncOk=true, deviceOk=true;
struct FsBlockDeviceInterface {
 bool stopped=false;
 bool syncDevice() { events.push_back(4); return deviceOk; }
 void end() { stopped=true; }
};
struct File {
 operator bool() const { return openOk; }
 bool sync() { events.push_back(2); return syncOk; }
 ~File() { events.push_back(3); }
};
struct Volume {
 File open(const char*,int) { events.push_back(1); return {}; }
 void end() { events.push_back(5); }
};
struct Sd:Volume {
 FsBlockDeviceInterface cardObject;
 bool present=true;
 FsBlockDeviceInterface* card() { return present ? &cardObject : nullptr; }
 void end() { Volume::end(); cardObject.end(); }
};
struct SDCardManager {
 bool initialized=true;
 uint64_t cachedTotalBytes=1234, cachedUsedBytes=100;
 bool cachedUsedBytesValid=true;
 Sd sd;
 Volume& vol() { return sd; }
 FsBlockDeviceInterface* detachFilesystemForRawAccess();
};
'''
test = r'''
int main() {
 SDCardManager manager;
 auto* card=manager.detachFilesystemForRawAccess();
 assert(card==manager.sd.card());
 assert(!card->stopped);
 assert((events==std::vector<int>{1,2,3,4,5}));
 assert(!manager.initialized && !manager.cachedTotalBytes && !manager.cachedUsedBytes && !manager.cachedUsedBytesValid);
 events.clear();
 assert(!manager.detachFilesystemForRawAccess() && events.empty());
 for(int failure=0; failure<4; ++failure) {
  SDCardManager failing;
  openOk=failure!=0; syncOk=failure!=1; deviceOk=failure!=2;
  failing.sd.present=failure!=3;
  events.clear();
  assert(!failing.detachFilesystemForRawAccess());
  assert(failing.initialized && failing.cachedTotalBytes==1234);
  assert(!failing.sd.cardObject.stopped);
  for(auto e:events) assert(e!=5);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='usb-spi-handoff-') as tmp:
    path=Path(tmp)
    (path/'test.cpp').write_text(fixture+s[start:end]+test)
    subprocess.run(['c++','-std=c++20','-fsanitize=address,undefined',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('SPI USB handoff: sync/close before unmount, card remains active, failures retain mount, repeated detach rejected')
