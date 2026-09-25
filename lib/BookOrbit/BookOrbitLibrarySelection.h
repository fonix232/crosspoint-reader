#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace bookorbit {
// Bounded, allocation-free settings snapshot, copied once per network session.
struct LibrarySelection {
  static constexpr size_t MAX = 32;
  bool all = true;
  uint8_t count = 0;
  std::array<uint32_t, MAX> ids{};

  bool contains(uint32_t id) const {
    return all || std::find(ids.begin(), ids.begin() + count, id) != ids.begin() + count;
  }
  bool toggle(uint32_t id) {
    if (!id) return false;
    if (all) {
      all = false;
      count = 0;
    }
    auto end = ids.begin() + count;
    auto found = std::find(ids.begin(), end, id);
    if (found != end) {
      std::move(found + 1, end, found);
      ids[--count] = 0;
    } else {
      if (count == MAX) return false;
      ids[count++] = id;
      std::sort(ids.begin(), ids.begin() + count);
    }
    return true;
  }
  std::string key() const {
    if (all) return "all";
    std::string out = "selected";
    for (unsigned i = 0; i < count; ++i) out += "," + std::to_string(ids[i]);
    return out;
  }
};
}  // namespace bookorbit
