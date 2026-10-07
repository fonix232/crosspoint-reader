#!/usr/bin/env python3
"""Exercise production CPU idle policy and PaperS3 analog USB detection."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'lib/hal/HalPowerManager.cpp').read_text()
start = source.index('void HalPowerManager::setPowerSaving(')
end = source.index('void HalPowerManager::startDeepSleep(', start)
policy = source[start:end]
with tempfile.TemporaryDirectory() as directory:
    d = Path(directory)
    code = r'''
#include <cassert>
#define LOG_DBG(...) ((void)0)
constexpr int portMAX_DELAY=-1;
inline bool xSemaphoreTake(int,int){return true;}
inline void xSemaphoreGive(int){}
constexpr int WIFI_MODE_NULL=0;
struct { int mode=1; int getMode(){return mode;} } WiFi;
int frequency=240;
bool setCpuFrequencyMhz(int value){frequency=value;return true;}
struct HalPowerManager {
 int normalFreq=240;
 bool isLowPower=false;
 enum LockMode {None, NormalSpeed};
 LockMode currentLockMode=None;
 int modeMutex=0;
 static constexpr int LOW_POWER_FREQ=TEST_IDLE_FREQ;
 void setPowerSaving(bool);
};
''' + policy + r'''
int main(){
 HalPowerManager p;
 p.setPowerSaving(true);
 assert(frequency==(TEST_IDLE_FREQ>=80?TEST_IDLE_FREQ:240));
 p.currentLockMode=HalPowerManager::NormalSpeed;p.setPowerSaving(true);assert(frequency==240);
 p.currentLockMode=HalPowerManager::None;WiFi.mode=0;p.setPowerSaving(true);assert(frequency==TEST_IDLE_FREQ);
 p.setPowerSaving(false);assert(frequency==240);
}
'''
    (d/'policy.cpp').write_text(code)
    for frequency in (10,80):
        subprocess.run(['c++','-std=c++17',f'-DTEST_IDLE_FREQ={frequency}',str(d/'policy.cpp'),'-o',str(d/'policy')],check=True)
        subprocess.run([str(d/'policy')],check=True)
    (d/'Arduino.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cassert>
constexpr int OUTPUT=1,LOW=0,HIGH=1;
inline unsigned long now=0;
inline unsigned samples=0,pulses=0;
inline uint16_t voltage=450;
inline unsigned long millis(){return now;}
inline uint16_t analogReadMilliVolts(int pin){assert(pin==5);++samples;return voltage;}
inline void pinMode(int pin,int mode){assert(pin==44&&mode==OUTPUT);}
inline void digitalWrite(int pin,int value){assert(pin==44);if(value==HIGH)++pulses;}
inline void delay(unsigned ms){now+=ms;}
''')
    board = root/'freeink-sdk/libs/hardware/BoardPaperS3'
    (d/'usb.cpp').write_text(r'''
#include <BoardPaperS3.h>
int main(){
 assert(BoardPaperS3::usbMillivolts()==450 && samples==1);
 voltage=0;now=99;assert(BoardPaperS3::usbMillivolts()==450 && samples==1);
 now=100;assert(BoardPaperS3::usbMillivolts()==0 && samples==2);
 voltage=450;now=200;assert(BoardPaperS3::usbMillivolts()==450 && samples==3);
 BoardPaperS3::powerOff();assert(pulses==5 && now==700);
}
''')
    subprocess.run(['c++','-std=c++17','-I'+str(d),'-I'+str(board/'include'),str(d/'usb.cpp'),str(board/'src/BoardPaperS3.cpp'),'-o',str(d/'usb')],check=True)
    subprocess.run([str(d/'usb')],check=True)
print('Power policy: 80 MHz Wi-Fi idle, sub-80 MHz protection, render lock restore, analog USB cache and power-off pulses passed')
