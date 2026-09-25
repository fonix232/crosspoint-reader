#pragma once

namespace reset_sleep {

enum class BootCause { External, Software, Fault, Other };

constexpr bool shouldSleep(bool wasAwake, BootCause cause, bool maintenanceRestart) {
  return wasAwake && cause == BootCause::External && !maintenanceRestart;
}

constexpr bool isSleepWake(bool wasAwake, BootCause cause, bool maintenanceRestart, bool tapWake) {
  return !wasAwake && !maintenanceRestart &&
         (cause == BootCause::External || (cause == BootCause::Software && tapWake));
}

}  // namespace reset_sleep
