#!/usr/bin/env python3
"""Compile the production logger against deterministic host power/storage doubles."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    p = pathlib.Path(tmp)
    (p/'activities').mkdir()
    common = r'''
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <ctime>
inline unsigned long tick=0;
inline unsigned long millis(){return tick;}
inline std::string data;
inline int usb=1, writes=0;
inline bool locked=false;
struct BoardConfig {static bool isM5PaperS3(){return true;}};
struct Clock {bool utcNow(time_t& t){t=1800000000;return true;}};
inline Clock halClock;
struct Gpio {bool isUsbConnected(){return usb;}};
inline Gpio gpio;
struct Power {
 struct D {int millivolts=4100,estimatedPercent=90,displayedPercent=88,usbConnected=1,usbMillivolts=280,charging=1,cpuMhz=80,wifiMode=0,wifiConnected=0,epdPower=0,epdOutputEnable=0;};
 uint64_t idleSleepUs=0;uint32_t idleSleepCount=0;
 void getBatteryPercentage(){}
 D diagnostics(){D d;d.usbConnected=usb;return d;}
};
inline Power powerManager;
struct Serial {void println(const char*){}};
inline Serial logSerial;
struct File {
 operator bool(){return true;}
 size_t size(){return data.size();}
 void print(const char* s){data+=s;}
 size_t write(const uint8_t* s,size_t n){data.append((const char*)s,n);++writes;return n;}
 void flush(){}
};
struct StorageType {
 bool ready(){return true;}
 bool ensureDirectoryExists(const char*){return true;}
 File open(const char*,int){return {};}
 void readFileToStream(const char*,Serial&,int){}
};
inline StorageType Storage;
constexpr int O_WRONLY=1,O_CREAT=2,O_APPEND=4;
inline uint32_t esp_random(){return 123;}
inline int esp_reset_reason(){return 1;}
#define LOG_ERR(...) ((void)0)
struct RenderLock {
 enum class Mode{Try};
 RenderLock(Mode){}
 bool ownsLock(){return !locked;}
};
'''
    (p/'common.h').write_text(common)
    for name in ['BoardConfig.h','HalClock.h','HalGPIO.h','HalPowerManager.h','HalStorage.h','Logging.h','esp_system.h','activities/RenderLock.h']:
        (p/name).write_text('#include "common.h"\n')
    (p/'test.cpp').write_text(r'''
#include "common.h"
#include "PowerLog.h"
#include <cassert>
int main(){
 power_log::record("boot",false);
 assert(writes==1 && data.find("session,utc,")==0);
 tick=250;power_log::poll(false);assert(writes==1);
 usb=0;tick=500;power_log::poll(true);
 assert(writes==2 && data.find(",usb_unplug,")!=std::string::npos);
 tick=60000;power_log::poll(true);assert(writes==2);
 tick=60500;power_log::poll(true);assert(writes==3);
 usb=1;locked=true;tick=61000;power_log::poll(false);assert(writes==3);
 locked=false;tick=61250;power_log::poll(false);
 assert(writes==4 && data.find(",usb_plug,")!=std::string::npos);
 power_log::record("sleep_ready",false);assert(writes==5);
 data.resize(16*1024*1024);tick=130000;power_log::poll(false);assert(writes==5);
}
''')
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(p),'-I'+str(root/'src/diagnostics'),str(root/'src/diagnostics/PowerLog.cpp'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS: persistent power records, interval, USB edges, render deferral, size cap')
