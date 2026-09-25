#pragma once
#include <HalStorage.h>

#include <iostream>
#include <limits>

namespace serialization {
// Cache strings are metadata, paths or individual words, never whole chapters.
// Bound allocations even when a corrupt length still fits inside a large file.
inline constexpr uint32_t MAX_STRING_BYTES = 16 * 1024;
template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
bool tryWritePod(HalFile& file, const T& value) {
  return file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

template <typename T>
bool readPod(std::istream& is, T& value) {
  value = T{};
  if (!is.read(reinterpret_cast<char*>(&value), sizeof(T))) {
    value = T{};
    return false;
  }
  return true;
}

template <typename T>
bool readPod(HalFile& file, T& value) {
  value = T{};
  if (file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) != sizeof(T)) {
    value = T{};
    return false;
  }
  return true;
}

template <typename T>
bool tryReadPod(HalFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

inline bool tryWriteString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  return tryWritePod(file, len) && (len == 0 || file.write(reinterpret_cast<const uint8_t*>(s.data()), len) == len);
}

inline bool readString(std::istream& is, std::string& s) {
  s.clear();
  uint32_t len = 0;
  if (!readPod(is, len) || len > MAX_STRING_BYTES) return false;
  s.resize(len);
  if (len && !is.read(&s[0], len)) {
    s.clear();
    return false;
  }
  return true;
}

inline bool readString(HalFile& file, std::string& s) {
  s.clear();
  uint32_t len = 0;
  if (!readPod(file, len) || len > MAX_STRING_BYTES) return false;
  const auto pos = file.position();
  const auto size = file.size();
  if (pos > size || len > size - pos) return false;
  s.resize(len);
  if (len && file.read(&s[0], len) != static_cast<int>(len)) {
    s.clear();
    return false;
  }
  return true;
}

inline bool tryReadString(HalFile& file, std::string& s, const size_t maxLength) {
  uint32_t len = 0;
  if (!tryReadPod(file, len)) return false;
  const int remaining = file.available();
  if (static_cast<size_t>(len) > maxLength || static_cast<size_t>(len) > s.max_size() ||
      len > static_cast<uint32_t>(std::numeric_limits<int>::max()) || remaining < 0 ||
      len > static_cast<uint32_t>(remaining)) {
    return false;
  }
  s.resize(len);
  return len == 0 || file.read(&s[0], len) == static_cast<int>(len);
}
}  // namespace serialization
