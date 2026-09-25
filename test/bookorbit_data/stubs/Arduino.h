#pragma once
#include <cstdint>
inline uint32_t fakeMillis = 0;
inline uint32_t millis() { return fakeMillis; }
inline void delay(unsigned long ms) { fakeMillis += ms; }
inline void configTzTime(const char*, const char*, const char*) {}
