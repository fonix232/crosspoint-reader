#include <gtest/gtest.h>

#include "ChapterXPathResolver.h"
#include "HighlightTextResolver.h"
namespace {
std::shared_ptr<Epub> book(const std::string& xml) { return std::make_shared<Epub>(std::vector<std::string>{xml}); }
}  // namespace
TEST(HighlightTextResolver, ResolvesWhitespaceInlineMarkupAndUnicodeToOriginalOffsets) {
  auto epub = book("<html><body><p>Start  café <em>nested</em> text.</p></body></html>");
  uint32_t start = 0, end = 0;
  ASSERT_TRUE(resolveHighlightText(epub, 0, 0, "Start", "café nested", start, end));
  EXPECT_EQ(start, 7u);
  EXPECT_EQ(end, 18u);
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, start),
            "/body/DocFragment[1]/body/p[1]/text()[1].7");
}
TEST(HighlightTextResolver, UsesPageAnchorToAvoidAnEarlierRepeatedPhrase) {
  auto epub = book("<html><body><p>same text</p><p>same text</p></body></html>");
  uint32_t start = 0, end = 0;
  ASSERT_TRUE(resolveHighlightText(epub, 0, 9, "", "same text", start, end));
  EXPECT_EQ(start, 9u);
  EXPECT_EQ(end, 18u);
}
TEST(HighlightTextResolver, RejectsMissingTextInsteadOfApproximating) {
  auto epub = book("<html><body><p>different text</p></body></html>");
  uint32_t start = 0, end = 0;
  EXPECT_FALSE(resolveHighlightText(epub, 0, 0, "", "selected text", start, end));
  EXPECT_FALSE(resolveHighlightText(epub, 5, 0, "", "different", start, end));
}
TEST(HighlightTextResolver, JoinsHyphenatedWordsAndSkipsHiddenContent) {
  auto epub = book("<html><body><p><rp>hidden</rp>hyphenation works</p></body></html>");
  uint32_t start = 0, end = 0;
  ASSERT_TRUE(resolveHighlightText(epub, 0, 0, "", "hyphen- ation", start, end));
  EXPECT_EQ(start, 0u);
  EXPECT_EQ(end, 11u);
}
