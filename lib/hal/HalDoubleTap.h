#pragma once

// PaperS3-only casing taps. Other boards compile no-op bodies.
namespace HalDoubleTap {
void begin(bool enabled);
bool poll(bool enabled);
// Called after the screen, reading state and SD card have been prepared for sleep.
// Returns only when disabled or unavailable, so ordinary deep sleep can take over.
void sleep(bool usbConnected);
// Read and clear the retained marker for USB light-sleep -> software-reboot wake.
bool consumeWake();
}  // namespace HalDoubleTap
