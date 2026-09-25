#include "HighlightTextResolver.h"

#include <Epub/VisibleTextUtils.h>
#include <Memory.h>
#include <Print.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <cstring>
#include <memory>

namespace {
bool ignore(uint32_t c) {
  return c <= 32 || c == 0xa0 || c == 0xad || c == 0x200b || c == '-' || (c >= 0x2000 && c <= 0x200a);
}
struct Matcher final : Print {
  XML_Parser parser = nullptr;
  std::unique_ptr<uint32_t[]> pattern, failure, offsets;
  size_t patternSize = 0;
  size_t matched = 0, seen = 0, prefixSize = 0;
  uint32_t minimum = 0, position = 0, start = 0, end = 0;
  int body = 0, hidden = 0, paragraph = 0;
  bool found = false, good = true;
  Matcher() {
    pattern = makeUniqueNoThrow<uint32_t[]>(2048);
    failure = makeUniqueNoThrow<uint32_t[]>(2048);
    offsets = makeUniqueNoThrow<uint32_t[]>(2048);
  }
  ~Matcher() override { destroyXmlParser(parser); }
  void append(const std::string& text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
    while (*p && patternSize < 2048) {
      const uint32_t c = utf8NextCodepoint(&p);
      if (!ignore(c)) pattern[patternSize++] = c;
    }
    if (*p) good = false;
  }
  bool begin(const std::string& prefix, const std::string& text) {
    if (!pattern || !failure || !offsets) return false;
    append(prefix);
    prefixSize = patternSize;
    append(text);
    if (!good || patternSize == prefixSize) return false;
    for (size_t i = 1, j = 0; i < patternSize; ++i) {
      while (j && pattern[i] != pattern[j]) j = failure[j - 1];
      if (pattern[i] == pattern[j]) ++j;
      failure[i] = j;
    }
    parser = XML_ParserCreate(nullptr);
    if (!parser) return false;
    XML_SetUserData(parser, this);
    XML_SetElementHandler(
        parser,
        [](void* ctx, const char* raw, const char**) {
          auto& s = *static_cast<Matcher*>(ctx);
          const char* name = strrchr(raw, ':');
          name = name ? name + 1 : raw;
          if (VisibleTextUtils::equalsTag(name, "body")) ++s.body;
          if (s.hidden || VisibleTextUtils::isNonVisibleElement(name)) ++s.hidden;
          if (VisibleTextUtils::equalsTag(name, "p") || VisibleTextUtils::equalsTag(name, "li")) ++s.paragraph;
        },
        [](void* ctx, const char* raw) {
          auto& s = *static_cast<Matcher*>(ctx);
          const char* name = strrchr(raw, ':');
          name = name ? name + 1 : raw;
          if (s.hidden) --s.hidden;
          if (VisibleTextUtils::equalsTag(name, "p") || VisibleTextUtils::equalsTag(name, "li")) --s.paragraph;
          if (VisibleTextUtils::equalsTag(name, "body")) --s.body;
        });
    XML_SetCharacterDataHandler(parser, [](void* ctx, const char* data, int size) {
      auto& s = *static_cast<Matcher*>(ctx);
      if (!s.body || s.hidden || s.paragraph <= 0 || s.found) return;
      const auto* p = reinterpret_cast<const unsigned char*>(data);
      const auto* stop = p + size;
      while (p < stop && !s.found) {
        const auto c = utf8NextCodepoint(&p);
        const auto offset = s.position++;
        if (offset < s.minimum || ignore(c)) continue;
        if (offset - s.minimum > 20000) {
          s.good = false;
          return;
        }
        s.offsets[s.seen++ % s.patternSize] = offset;
        while (s.matched && c != s.pattern[s.matched]) s.matched = s.failure[s.matched - 1];
        if (c == s.pattern[s.matched]) ++s.matched;
        if (s.matched == s.patternSize) {
          s.start = s.offsets[(s.seen - s.patternSize + s.prefixSize) % s.patternSize];
          s.end = offset + 1;
          s.found = true;
        }
      }
    });
    return true;
  }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* bytes, size_t size) override {
    if (!found && good && XML_Parse(parser, reinterpret_cast<const char*>(bytes), size, XML_FALSE) == XML_STATUS_ERROR)
      good = false;
    return size;
  }
};
}  // namespace
bool resolveHighlightText(const std::shared_ptr<Epub>& epub, int spine, uint32_t pageOffset, const std::string& prefix,
                          const std::string& text, uint32_t& start, uint32_t& end) {
  if (!epub || spine < 0 || spine >= epub->getSpineItemsCount()) return false;
  // Three bounded 2048-element work arrays live only during selection; placing
  // them on the heap avoids overflowing the embedded UI task's stack.
  auto matcher = makeUniqueNoThrow<Matcher>();
  if (!matcher || !matcher->begin(prefix, text)) return false;
  matcher->minimum = pageOffset;
  if (!epub->readItemContentsToStream(epub->getSpineItem(spine).href, *matcher, 1024) || !matcher->good ||
      !matcher->found)
    return false;
  start = matcher->start;
  end = matcher->end;
  return true;
}
