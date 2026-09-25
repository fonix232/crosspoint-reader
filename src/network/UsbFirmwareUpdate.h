#pragma once

namespace usb_firmware {
// Main-task only. Returns true for firmware protocol commands, including errors.
bool handleCommand(const char* command);
}  // namespace usb_firmware
