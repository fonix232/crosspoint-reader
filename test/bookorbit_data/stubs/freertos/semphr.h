#pragma once
#include <mutex>
using SemaphoreHandle_t = std::mutex*;
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  static std::mutex mutex;
  return &mutex;
}
inline int xSemaphoreTake(SemaphoreHandle_t mutex, int) {
  mutex->lock();
  return 1;
}
inline void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
