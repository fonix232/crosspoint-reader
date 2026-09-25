#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace usb_firmware {
constexpr size_t CHUNK_SIZE = 128;  // Fits the default 256-byte CDC receive queue.
constexpr uint32_t MIN_IMAGE_SIZE = 64 * 1024;
struct UploadRequest {
  uint32_t size = 0;
  uint8_t sha[32] = {};
};
inline int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
inline bool parseUpload(const char* text, uint32_t limit, UploadRequest& out) {
  uint32_t size = 0;
  if (*text < '0' || *text > '9') return false;
  while (*text >= '0' && *text <= '9') {
    const uint32_t digit = *text++ - '0';
    if (size > UINT32_MAX / 10 || (size == UINT32_MAX / 10 && digit > UINT32_MAX % 10)) return false;
    size = size * 10 + digit;
  }
  if (*text++ != ' ' || size < MIN_IMAGE_SIZE || size > limit || strlen(text) != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const int hi = hexDigit(text[2 * i]), lo = hexDigit(text[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.sha[i] = (hi << 4) | lo;
  }
  out.size = size;
  return true;
}

// Discard overlong or binary command lines through their terminating newline.
class CommandLine {
  char buffer[128] = {};
  size_t length = 0;
  bool invalid = false;

 public:
  const char* feed(int byte) {
    if (byte == '\n') {
      buffer[length] = 0;
      const bool valid = !invalid;
      length = 0;
      invalid = false;
      return valid ? buffer : nullptr;
    }
    if (byte == '\r') return nullptr;
    if (byte < 32 || byte > 126 || length == sizeof(buffer) - 1) invalid = true;
    if (!invalid) buffer[length++] = static_cast<char>(byte);
    return nullptr;
  }
};
}  // namespace usb_firmware
