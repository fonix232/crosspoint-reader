#pragma once

#include <cstdint>

namespace library {

// Reversal is its own inverse: the same mapping restores selection on Back.
constexpr int orderedPosition(int row, int count, bool reversed) {
  if (row < 0 || row >= count) return -1;
  return reversed ? count - 1 - row : row;
}

struct GroupRange {
  int start = 0;
  int size = 0;
};

// Group starts are positions in the active order, including descending order.
constexpr GroupRange groupRange(const uint16_t* starts, int groups, int books, int group) {
  if (!starts || group < 0 || group >= groups) return {};
  const int start = starts[group];
  const int end = group + 1 < groups ? starts[group + 1] : books;
  if (start < 0 || start >= books || end <= start || end > books) return {};
  return {start, end - start};
}

}  // namespace library
