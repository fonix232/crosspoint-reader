#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "DebugInput.h"

#define LOG_SERIAL_HAS_TX_TIMEOUT 1
inline unsigned long fakeNow = 1000;
inline unsigned long millis() { return fakeNow; }
inline void delay(unsigned long duration) { fakeNow += duration; }
inline bool renderAvailable = true;
struct RenderLock {
  enum class Mode { Blocking, Try };
  explicit RenderLock(Mode = Mode::Blocking) {}
  bool ownsLock() const { return renderAvailable; }
};
struct Manager {
  bool idle = true;
  bool debugRenderIdle() const { return idle; }
};
inline Manager activityManager;
class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  Orientation orientation = Portrait;
  Orientation getOrientation() const { return orientation; }
  int getScreenWidth() const { return (orientation == Portrait || orientation == PortraitInverted) ? 480 : 800; }
  int getScreenHeight() const { return (orientation == Portrait || orientation == PortraitInverted) ? 800 : 480; }
  int getDisplayWidth() const { return 800; }
  int getDisplayHeight() const { return 480; }
};
class HalDisplay {
 public:
  uint8_t pixels[2] = {0x80, 0x40};
  const uint8_t* getFrameBuffer() const { return pixels; }
  uint32_t getBufferSize() const { return 2; }
  unsigned getDisplayWidth() const { return 8; }
  unsigned getDisplayHeight() const { return 2; }
  unsigned getDisplayWidthBytes() const { return 1; }
};
struct GPIO {
  DebugInput input;
  bool touch = true, home = true;
  DebugInput& debugInput() { return input; }
  bool hasTouch() const { return touch; }
  bool hasHomeKey() const { return home; }
  void setDebugGeometry(int, int) {}
};
inline GPIO gpio;
struct SerialFake {
  unsigned timeout = 1;
  bool shortWrite = false;
  std::string output;
  operator bool() const { return true; }
  void setTxTimeoutMs(unsigned value) { timeout = value; }
  void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char text[256];
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    output += text;
  }
  size_t write(const uint8_t* buffer, size_t size) {
    if (shortWrite) return 0;
    output.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }
};
inline SerialFake logSerial;

struct PowerFake {
  struct Diagnostics {
    int millivolts = 4010, estimatedPercent = 80, displayedPercent = 86;
    int cpuMhz = 80, wifiMode = 0, wifiConnected = 0, usbConnected = 1;
    int charging = 1, epdPower = 0, epdOutputEnable = 0, usbMillivolts = 420;
  };
  Diagnostics diagnostics() const { return {}; }
};
inline PowerFake powerManager;
