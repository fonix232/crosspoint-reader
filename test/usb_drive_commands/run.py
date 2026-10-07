#!/usr/bin/env python3
"""Exercise the production USB-drive command handler without hardware."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'src/main.cpp').read_text()
handler = source[source.index('static bool handleUsbDriveCommand('):source.index('\nvoid loop() {')]
code = r'''
#include <cassert>
#include <cstring>
#include <string>
#include <cstdio>
enum class UsbDriveState {Unsupported, WaitingForHost, Connected, Ejected, Disconnected, IoError};
struct Manager {
 bool active=false, startOk=true; int starts=0;
 bool requiresExclusiveStorageLoop() {return active;}
 bool goToUsbDrive() {++starts; return startOk;}
} activityManager;
struct StorageFake {
 UsbDriveState state=UsbDriveState::Unsupported;int stops=0;
 UsbDriveState usbDriveState() {return state;}
 void endUsbDrive() {++stops;}
} Storage;
struct SerialFake {
 std::string output;bool flushed=false;
 void printf(const char* fmt,const char* state) {char b[100];std::snprintf(b,sizeof(b),fmt,state);output=b;}
 void println(const char* s) {output=std::string(s)+"\n";}
 void flush() {flushed=true;}
} logSerial;
int restarts=0;
void restartToHomeAfterStorageHandoff(){assert(logSerial.flushed);assert(Storage.stops>0);++restarts;}
'''
code += handler
code += r'''
int main() {
 assert(!handleUsbDriveCommand("CMD:FW_INFO"));
 assert(!handleUsbDriveCommand("CMD:SCREENSHOT"));
 assert(!handleUsbDriveCommand("CMD:USBISH ON"));
#if FREEINK_CAP_USB_MSC
 assert(handleUsbDriveCommand("CMD:USB STATUS"));assert(logSerial.output=="USB:OFF\n");
 handleUsbDriveCommand("CMD:USB OFF");assert(restarts==0 && Storage.stops==0);
 handleUsbDriveCommand("CMD:USB ON");assert(activityManager.starts==1 && logSerial.output=="USB:STARTING\n");
 activityManager.startOk=false;
 handleUsbDriveCommand("CMD:USB ON");assert(logSerial.output=="USB:ERROR START_FAILED\n");
 activityManager.active=true;
 Storage.state=UsbDriveState::Connected;
 handleUsbDriveCommand("CMD:USB ON");assert(activityManager.starts==2 && logSerial.output=="USB:CONNECTED\n");
 for(auto state:{UsbDriveState::Connected,UsbDriveState::IoError}) {
  Storage.state=state;handleUsbDriveCommand("CMD:USB OFF");
  assert(logSerial.output=="USB:ERROR EJECT_REQUIRED\n" && restarts==0 && Storage.stops==0);
 }
 for(auto state:{UsbDriveState::WaitingForHost,UsbDriveState::Ejected,UsbDriveState::Disconnected,UsbDriveState::Unsupported}) {
  Storage.state=state;int before=restarts;handleUsbDriveCommand("CMD:USB OFF");
  assert(restarts==before+1 && logSerial.output=="USB:RESTARTING\n");
 }
 handleUsbDriveCommand("CMD:USB OOPS");assert(logSerial.output=="USB:ERROR EXPECT_ON_OFF_STATUS\n");
#else
 handleUsbDriveCommand("CMD:USB ON");assert(logSerial.output=="USB:ERROR UNSUPPORTED\n");
 assert(activityManager.starts==0 && Storage.stops==0 && restarts==0);
#endif
}
'''
assert source.index('if (handleUsbDriveCommand(command))') < source.index('if (activityManager.requiresExclusiveStorageLoop())',source.index('\nvoid loop()'))
with tempfile.TemporaryDirectory() as tmp:
    cpp,exe=Path(tmp)/'commands.cpp',Path(tmp)/'commands'
    cpp.write_text(code)
    for supported in (0,1):
        subprocess.run(['c++','-std=c++20',f'-DFREEINK_CAP_USB_MSC={supported}',str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
print('USB commands: on/off/status, idempotence, mounted/error refusal, safe exit and unsupported boards passed')
