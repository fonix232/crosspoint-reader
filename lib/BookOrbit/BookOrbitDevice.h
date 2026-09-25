#pragma once

#include <string>

namespace bookorbit {

// Human-readable device name reported to BookOrbit with every progress update:
// the board model plus the last three octets of the factory MAC, e.g.
// "PaperS3-cdef01", so two devices of one model stay apart in the device list.
const std::string& deviceName();

}  // namespace bookorbit
