#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
struct TestDisk {
  std::vector<uint8_t> bytes;
  bool removed = false;
  int failRead = -1;
  bool failSeek = false;
};
inline auto disk = std::make_shared<TestDisk>();
class HalFile {
  size_t pos = 0;
  bool hasImpl = false;
  bool opened = false;

 public:
  HalFile() = default;
  explicit HalFile(bool openSucceeded) : hasImpl(true), opened(openSucceeded) {}
  explicit operator bool() const { return opened; }
  int read(void* out, size_t n) {
    assert(hasImpl);
    if (!opened || disk->removed || disk->failRead == 0) return -1;
    if (disk->failRead > 0) --disk->failRead;
    n = std::min(n, disk->bytes.size() - std::min(pos, disk->bytes.size()));
    memcpy(out, disk->bytes.data() + pos, n);
    pos += n;
    return n;
  }
  size_t write(const void*, size_t n) {
    assert(hasImpl);
    return opened && !disk->removed ? n : 0;
  }
  size_t size() {
    assert(hasImpl);
    return opened ? disk->bytes.size() : 0;
  }
  size_t position() {
    assert(hasImpl);
    return pos;
  }
  bool seek(size_t n) {
    assert(hasImpl);
    if (!opened || disk->removed || disk->failSeek || n > size()) return false;
    pos = n;
    return true;
  }
  void close() {
    assert(hasImpl);
    opened = false;
  }
};
struct TestStorage {
  bool openFileForRead(const char*, const std::string&, HalFile& f) {
    f = HalFile{!disk->removed};
    return !disk->removed;
  }
  bool openFileForWrite(const char*, const std::string&, HalFile& f) {
    f = HalFile{!disk->removed};
    return !disk->removed;
  }
  bool exists(const char*) { return false; }
  bool remove(const char*) { return true; }
};
inline TestStorage Storage;
