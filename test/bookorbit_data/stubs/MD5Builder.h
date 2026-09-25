#pragma once
#include <stdexcept>
#include <string>
// Storage tests provide explicit identities. Never pretend to test the ESP32
// SDK's MD5 implementation with a substitute hash.
class MD5Builder {
 public:
  void begin() { throw std::logic_error("MD5 is outside the storage test adapter"); }
  void add(const char*) {}
  void calculate() {}
  std::string toString() const { return {}; }
};
