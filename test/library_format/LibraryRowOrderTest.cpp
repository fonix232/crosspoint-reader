#include <gtest/gtest.h>

#include <array>

#include "LibraryRowOrder.h"

TEST(LibraryRowOrder, AscendingAndDescendingViewsResolveTheDisplayedGroup) {
  constexpr std::array<int, 4> starts{0, 2, 7, 10};
  constexpr std::array<int, 4> sizes{2, 5, 3, 8};
  constexpr int expectedStarts[2][4] = {{0, 2, 7, 10}, {10, 7, 2, 0}};
  constexpr int expectedSizes[2][4] = {{2, 5, 3, 8}, {8, 3, 5, 2}};
  for (int direction = 0; direction < 2; ++direction) {
    for (int row = 0; row < 4; ++row) {
      const int group = library::orderedPosition(row, 4, direction != 0);
      EXPECT_EQ(starts[group], expectedStarts[direction][row]);
      EXPECT_EQ(sizes[group], expectedSizes[direction][row]);
      // Returning from that series must select the same visible row.
      EXPECT_EQ(library::orderedPosition(group, 4, direction != 0), row);
    }
  }
}

TEST(LibraryRowOrder, EmptyAndStaleSelectionsAreRejected) {
  for (const bool reversed : {false, true}) {
    EXPECT_EQ(library::orderedPosition(0, 0, reversed), -1);
    EXPECT_EQ(library::orderedPosition(-1, 3, reversed), -1);
    EXPECT_EQ(library::orderedPosition(3, 3, reversed), -1);
    EXPECT_EQ(library::orderedPosition(0, 1, reversed), 0);
  }
}

TEST(LibraryRowOrder, AuthorRangesIncludeLastBookWithoutLeakingIntoNextAuthor) {
  // Three authors with 2, 1 and 3 books in ascending order.
  constexpr uint16_t ascending[] = {0, 2, 3};
  // Reversing the index reverses the authors and each author's books.
  constexpr uint16_t descending[] = {0, 3, 4};
  for (int group = 0; group < 3; ++group) {
    const auto forward = library::groupRange(ascending, 3, 6, group);
    const auto backward = library::groupRange(descending, 3, 6, 2 - group);
    EXPECT_EQ(forward.size, backward.size);
    EXPECT_EQ(forward.start + forward.size, 6 - backward.start);
  }
  EXPECT_EQ(library::groupRange(ascending, 3, 6, 2).size, 3);
  EXPECT_EQ(library::groupRange(descending, 3, 6, 2).size, 2);
}

TEST(LibraryRowOrder, InvalidAuthorRangesAreEmpty) {
  constexpr uint16_t starts[] = {0, 2, 3};
  EXPECT_EQ(library::groupRange(nullptr, 0, 0, 0).size, 0);
  EXPECT_EQ(library::groupRange(starts, 3, 6, -1).size, 0);
  EXPECT_EQ(library::groupRange(starts, 3, 6, 3).size, 0);
  EXPECT_EQ(library::groupRange(starts, 3, 2, 2).size, 0);
  EXPECT_EQ(library::groupRange(starts, 3, 1, 0).size, 0);
}
