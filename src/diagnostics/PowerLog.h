#pragma once

namespace power_log {
void record(const char* event, bool reading);
void poll(bool reading);
void exportLog();
}  // namespace power_log
