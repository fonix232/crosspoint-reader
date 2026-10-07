#!/usr/bin/env python3
"""Exercise real PaperS3 nap transport and extracted production HAL guards."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HAL = (ROOT / "lib/hal/HalPowerManager.cpp").read_text()


def function(signature):
    start = HAL.index(signature)
    pos = HAL.index("{", start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (HAL[end] == "{") - (HAL[end] == "}")
        end += 1
    return HAL[start:end]


with tempfile.TemporaryDirectory(prefix="papers3-idle-test-") as directory:
    temp = Path(directory)
    (temp / "BoardPaperS3.h").write_text(r'''
#pragma once
#include <cstdint>
inline uint16_t usbMv=0;
namespace BoardPaperS3 {
inline uint16_t usbMillivolts(){return usbMv;}
uint32_t idleSleep(uint32_t);
}
''')
    (temp / "esp_sleep.h").write_text(r'''
#pragma once
#include <cstdint>
constexpr int ESP_OK=0,ESP_SLEEP_WAKEUP_TIMER=4;
inline int enableResult=0,sleepResult=0,enableCalls=0,sleepCalls=0,disableCalls=0;
inline uint64_t timerUs=0;
inline int64_t clockUs=0;
inline int esp_sleep_enable_timer_wakeup(uint64_t us){++enableCalls;timerUs=us;return enableResult;}
inline int esp_light_sleep_start(){++sleepCalls;clockUs+=timerUs;return sleepResult;}
inline int esp_sleep_disable_wakeup_source(int source){assert(source==ESP_SLEEP_WAKEUP_TIMER);++disableCalls;return 0;}
''')
    (temp / "esp_timer.h").write_text(r'''
#pragma once
#include "esp_sleep.h"
inline int64_t esp_timer_get_time(){return clockUs;}
''')
    (temp / "transport.cpp").write_text(r'''
#include <cassert>
#include <BoardPaperS3.h>
#include <esp_sleep.h>
int main(){
 assert(BoardPaperS3::idleSleep(0)==0);
 assert(BoardPaperS3::idleSleep(51)==0);
 assert(enableCalls==0 && sleepCalls==0);
 usbMv=201;assert(BoardPaperS3::idleSleep(10)==0);assert(enableCalls==0);
 usbMv=0;enableResult=-1;assert(BoardPaperS3::idleSleep(10)==0);
 assert(enableCalls==1 && sleepCalls==0 && disableCalls==0);
 enableResult=0;sleepResult=-1;assert(BoardPaperS3::idleSleep(10)==0);
 assert(sleepCalls==1 && disableCalls==1);
 sleepResult=0;assert(BoardPaperS3::idleSleep(10)==10000);
 assert(timerUs==10000 && disableCalls==2);
 assert(BoardPaperS3::idleSleep(50)==50000);
 assert(disableCalls==3);
}
''')
    common = ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined"]
    board = ROOT / "freeink-sdk/libs/hardware/BoardPaperS3/src/BoardPaperS3Sleep.cpp"
    subprocess.run(common + ["-include", "cassert", "-I" + str(temp), str(temp / "transport.cpp"), str(board),
                             "-o", str(temp / "transport")], check=True)
    subprocess.run([str(temp / "transport")], check=True)
    (temp / "hal.cpp").write_text(r'''
#include <cassert>
#include <cstdint>
constexpr int WIFI_MODE_NULL=0,ESP_BT_CONTROLLER_STATUS_ENABLED=2;
struct {bool usb=false;bool isUsbConnected(){return usb;}} gpio;
struct {int mode=0;int getMode(){return mode;}} WiFi;
int btStatus=0;
bool btStarted(){return btStatus==ESP_BT_CONTROLLER_STATUS_ENABLED;}
unsigned long clockMs=0;
unsigned long millis(){return clockMs;}
struct BatteryStatus {bool millivoltsKnown=true;int millivolts=3300;};
BatteryStatus batteryStatus;
int reads=0;
struct BatteryMonitor {BatteryStatus readStatus()const{++reads;return batteryStatus;}};
namespace BoardPaperS3 {
unsigned calls=0;uint32_t result=10000;
uint32_t idleSleep(uint32_t ms){assert(ms>0 && ms<=10);++calls;return result;}
}
struct HalPowerManager {
 bool isLowPower=true;
 enum LockMode {None,NormalSpeed};
 LockMode currentLockMode=None;
 uint64_t idleSleepUs=0;
 uint32_t idleSleepCount=0;
 bool idleNap();
 bool batteryCritical();
};
''' + function("bool HalPowerManager::idleNap()") + "\n" +
        function("bool HalPowerManager::batteryCritical()") + r'''
int main(){
 HalPowerManager p;
#if FREEINK_DEVICE_PAPERS3
 p.isLowPower=false;assert(!p.idleNap());p.isLowPower=true;
 p.currentLockMode=HalPowerManager::NormalSpeed;assert(!p.idleNap());p.currentLockMode=HalPowerManager::None;
 gpio.usb=true;assert(!p.idleNap());gpio.usb=false;
 WiFi.mode=1;assert(!p.idleNap());WiFi.mode=0;
 btStatus=ESP_BT_CONTROLLER_STATUS_ENABLED;assert(!p.idleNap());btStatus=0;
 assert(BoardPaperS3::calls==0);
 BoardPaperS3::result=0;assert(!p.idleNap());assert(p.idleSleepCount==0);
 BoardPaperS3::result=10000;assert(p.idleNap());assert(p.idleSleepUs==10000 && p.idleSleepCount==1);
 assert(p.idleNap());assert(p.idleSleepUs==20000 && p.idleSleepCount==2);
 clockMs=29999;assert(!p.batteryCritical());assert(reads==0);
 clockMs=30000;assert(!p.batteryCritical());assert(reads==1);
 clockMs=59999;assert(!p.batteryCritical());assert(reads==1);
 clockMs=60000;assert(!p.batteryCritical());
 clockMs=90000;assert(p.batteryCritical());
 gpio.usb=true;assert(!p.batteryCritical());gpio.usb=false;
 clockMs=120000;assert(!p.batteryCritical());
 batteryStatus.millivolts=3400;clockMs=150000;assert(!p.batteryCritical());
 batteryStatus.millivolts=3399;clockMs=180000;assert(!p.batteryCritical());
 batteryStatus.millivoltsKnown=false;clockMs=210000;assert(!p.batteryCritical());
 batteryStatus.millivoltsKnown=true;clockMs=240000;assert(!p.batteryCritical());
 batteryStatus.millivolts=0;clockMs=270000;assert(!p.batteryCritical());
 batteryStatus.millivolts=3300;clockMs=300000;assert(!p.batteryCritical());
 clockMs=330000;assert(!p.batteryCritical());clockMs=360000;assert(p.batteryCritical());
#else
 (void)gpio;(void)WiFi;(void)WIFI_MODE_NULL;(void)ESP_BT_CONTROLLER_STATUS_ENABLED;
 assert(!p.idleNap());assert(!p.batteryCritical());
 assert(BoardPaperS3::calls==0 && reads==0);
#endif
}
''')
    for enabled in (0, 1):
        subprocess.run(common + [f"-DFREEINK_DEVICE_PAPERS3={enabled}", str(temp / "hal.cpp"), "-o", str(temp / "hal")], check=True)
        subprocess.run([str(temp / "hal")], check=True)

print("PaperS3 idle sleep: timer bounds/USB/failure cleanup, HAL radio/lock guards, counters, critical-voltage debounce passed")
