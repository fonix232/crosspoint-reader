#!/usr/bin/env python3
"""Check logging transport selection and MSC publication with USB already running."""
from pathlib import Path
import configparser
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
config = configparser.ConfigParser(interpolation=None)
config.read(root / 'platformio.ini')
for profile in ('x4pro', 'x4pro-gh_release', 'x4pro-gh_release_rc'):
    env = config['env:' + profile]
    assert '-DARDUINO_USB_MODE=0' in env['build_flags']
    assert '-DARDUINO_USB_MODE=1' in env['build_unflags']
main = (root / 'src/main.cpp').read_text()
assert 'logSerial.enableReboot(false);' in main
handoff = (root / 'src/platform/UsbSerialJtagHandoff.cpp').read_text()
assert 'FREEINK_CAP_USB_MSC && ARDUINO_USB_MODE' in handoff
source = (root / 'freeink-sdk/libs/hardware/UsbMassStorage/src/UsbMassStorage.cpp').read_text()
start = source.index('bool UsbMassStorage::begin(')
end = source.index('void UsbMassStorage::end()', start)
fixture = r'''
#include <atomic>
#include <initializer_list>
#include <cassert>
#include <cstdint>
struct FsBlockDeviceInterface { uint32_t sectorCount() { return 100; } } device;
enum class UsbMassStorageState { Idle, WaitingForHost };
struct UsbMassStorage {
 bool _active=false;
 std::atomic<UsbMassStorageState> _state{UsbMassStorageState::Idle};
 std::atomic<bool> _hostSeen{false};
 bool begin(FsBlockDeviceInterface*);
};
std::atomic<FsBlockDeviceInterface*> gDev{nullptr};
std::atomic<UsbMassStorage*> gOwner{nullptr};
constexpr uint16_t kBlockSize=512;
void mscStartStop() {} void mscRead() {} void mscWrite() {}
bool ready=false, beginOk=true, usbOk=true;
struct {
 bool present=false, read=false, write=false, identity=false;
 // Arduino's USBMSC::end() clears the identity strings; begin() must restore them.
 void vendorID(const char*) { identity=true; } void productID(const char*) {} void productRevision(const char*) {}
 void isWritable(bool) {} void onStartStop(void(*)()) {}
 void onRead(void(*)()) { read=true; } void onWrite(void(*)()) { write=true; }
 void mediaPresent(bool value) {
   // An already-enumerated host can query media immediately at this point.
   if(value) assert(ready && read && write && identity && gDev.load() && gOwner.load());
   present=value;
 }
 bool begin(uint32_t count,uint16_t size) { assert(!present && count==100 && size==512); ready=beginOk; return beginOk; }
 void end() { present=ready=read=write=identity=false; }
} gMsc;
template <typename Msc> void setIdentity(Msc& msc) { msc.vendorID("FreeInk"); msc.productID("SD Card"); msc.productRevision("1.0"); }
struct { bool begin() { assert(gMsc.present); return usbOk; } } USB;
'''
test = r'''
int main() {
 for (int failure : {0,1,2}) {
   UsbMassStorage storage;
   beginOk=failure!=1; usbOk=failure!=2;
   const bool ok=storage.begin(&device);
   assert(ok==(failure==0));
   assert(storage._active==ok && gMsc.present==ok);
   if(!ok) { assert(!gDev.load() && !gOwner.load()); }
   else { assert(!storage.begin(&device)); }
   gMsc.end(); gDev=nullptr; gOwner=nullptr;
 }
}
'''
with tempfile.TemporaryDirectory(prefix='usb-cdc-') as directory:
    work = Path(directory)
    (work / 'Arduino.h').write_text('''#pragma once
#include <cstdint>
#include <cstddef>
class Print { public: virtual size_t write(uint8_t) { return 1; } virtual size_t write(const uint8_t*,size_t n) { return n; } virtual void flush() {} };
class HardwareSerial { public: void begin(unsigned long) {} operator bool() const { return true; } };
class HWCDC : public HardwareSerial {};
class USBCDC : public HardwareSerial {};
#if ARDUINO_USB_CDC_ON_BOOT
#if ARDUINO_USB_MODE
inline HWCDC TestSerial;
#else
inline USBCDC TestSerial;
#endif
#else
inline HardwareSerial TestSerial;
#endif
#define Serial TestSerial
''')
    for header in ('HWCDC.h', 'USBCDC.h', 'HardwareSerial.h'):
        (work / header).write_text('#include <Arduino.h>\n')
    cpp = work / 'logging.cpp'
    cpp.write_text('#include "Logging.h"\n#include <type_traits>\nstatic_assert(std::is_same_v<decltype(logSerial), EXPECTED&>);\nint main() {}\n')
    for mode, cdc, expected in [(0,1,'USBCDC'), (1,1,'HWCDC'), (1,0,'HardwareSerial')]:
        subprocess.run(['c++', '-std=c++17', '-I'+str(work), '-I'+str(root/'lib/Logging'),
                        f'-DARDUINO_USB_MODE={mode}', f'-DARDUINO_USB_CDC_ON_BOOT={cdc}',
                        '-DEXPECTED='+expected, str(cpp), '-o', str(work/'logging')], check=True)
    cpp = work / 'msc.cpp'
    cpp.write_text(fixture + source[start:end] + test)
    subprocess.run(['c++','-std=c++17',str(cpp),'-o',str(work/'msc')],check=True)
    subprocess.run([str(work/'msc')],check=True)
print('PASS: CDC/HWCDC/UART logging types, X4 Pro profiles, MSC readiness and failure cleanup')
