#!/usr/bin/env python3
"""Exercise production Wi-Fi idle countdown and job-completion policy."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'src/bookorbit/BookOrbitAgent.cpp').read_text()
start = source.index('uint32_t BookOrbitAgent::idleSecondsLeft() const')
end = source.index('// --- reader integration', start)
countdown = source[start:end]
start = source.index('  touchActivity();', source.index('void BookOrbitAgent::runJob('))
end = source.index('  // Screens poll generation()', start)
completion = source[start:end]
# No background connection producer may remain just because the setting is on.
loop = source[source.index('void BookOrbitAgent::loop()'):source.index('void BookOrbitAgent::prepareForSleep()')]
assert 'stayConnected(' not in source and 'lastKeepAliveMs' not in loop
code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
uint32_t now=0;
unsigned long millis(){return now;}
struct { bool keepWifiOn=true; unsigned wifiIdleMinutes(){return 5;} } SETTINGS;
unsigned pending=0;
unsigned uxQueueMessagesWaiting(int){return pending;}
struct BookOrbitAgent {
 std::atomic<bool> wifiOwned{true},holdLink{false};
 std::atomic<uint32_t> lastActivityMs{0};
 int queue=0,offCalls=0;
 uint32_t idleSecondsLeft() const;
 void touchActivity();
 void wifiOff(){wifiOwned=false;++offCalls;}
 void finish();
};
''' + countdown + '\nvoid BookOrbitAgent::finish(){\n' + completion + r'''
}
int main(){
 BookOrbitAgent a;
 now=1000;a.finish();assert(a.holdLink && a.offCalls==0 && a.idleSecondsLeft()==300);
 now=121000;assert(a.idleSecondsLeft()==180);
 now=301000;assert(a.idleSecondsLeft()==0); // setting still On must not bypass expiry
 // Same decision used by idle housekeeping: expiration drops the hold and radio.
 if(!a.holdLink || a.idleSecondsLeft()==0){a.holdLink=false;a.wifiOff();}
 now=900000;assert(a.idleSecondsLeft()==0 && !a.wifiOwned);
 a.wifiOwned=true; a.finish();assert(a.idleSecondsLeft()==300); // new job renews
 SETTINGS.keepWifiOn=false;assert(a.idleSecondsLeft()==0); // turning off expires immediately
 a.finish();assert(!a.holdLink && !a.wifiOwned);
 a.wifiOwned=true;pending=1;a.finish();assert(a.wifiOwned); // queued work retains radio
 SETTINGS.keepWifiOn=true;pending=0;now=0xfffffff0;a.finish();
 now=0x000003d8;assert(a.idleSecondsLeft()==299); // MCU millis rollover
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: bounded hold, expiry, renewal, disabling, queued jobs, millis rollover')
