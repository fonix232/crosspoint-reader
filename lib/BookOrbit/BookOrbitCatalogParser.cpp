#include "BookOrbitCatalogParser.h"

#include <LibraryText.h>
#include <Utf8.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bookorbit {

namespace {
bool keyIs(const char* key, const size_t len, const char* name) {
  return len == strlen(name) && memcmp(key, name, len) == 0;
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool readHex4(const char* p, uint32_t& out) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    const int h = hexValue(p[i]);
    if (h < 0) return false;
    v = (v << 4) | static_cast<uint32_t>(h);
  }
  out = v;
  return true;
}

// Both current string-valued positions and legacy JSON numbers use the
// existing fixed-point cache representation. Bound the token before strtod;
// callbacks supply a length, not a guaranteed NUL-terminated string.
uint16_t parseSeriesPosition(const char* value, const size_t len) {
  char token[64];
  if (len == 0 || len >= sizeof(token)) return 0;
  memcpy(token, value, len);
  token[len] = '\0';
  char* end = nullptr;
  const double position = strtod(token, &end);
  if (end != token + len || !std::isfinite(position) || position <= 0) return 0;
  return position >= 6553.5 ? 65535 : static_cast<uint16_t>(position * 10 + 0.5);
}

// Folded sort/search key, bounded to the record's prefix slot.
void storeFold(std::string_view text, char* out) {
  const std::string folded = library::fold(text);
  copyUtf8Bounded(folded, out, CATALOG_FOLD_BYTES);
}
}  // namespace

size_t decodeJsonUnicodeEscapes(char* text, const size_t len) {
  size_t r = 0;
  size_t w = 0;
  std::string scratch;
  while (r < len) {
    uint32_t cp;
    if (text[r] == '\\' && r + 6 <= len && text[r + 1] == 'u' && readHex4(text + r + 2, cp)) {
      r += 6;
      if (cp >= 0xD800 && cp <= 0xDBFF && r + 6 <= len && text[r] == '\\' && text[r + 1] == 'u') {
        uint32_t low;
        if (readHex4(text + r + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          r += 6;
        }
      }
      scratch.clear();
      utf8AppendCodepoint(cp, scratch);
      // The encoded form is at most as long as the 6-byte escape it replaces,
      // so writing in place never overtakes the read cursor.
      memcpy(text + w, scratch.data(), scratch.size());
      w += scratch.size();
      continue;
    }
    text[w++] = text[r++];
  }
  return w;
}

CatalogPageParser::CatalogPageParser(const ItemCallback onItem, void* ctx)
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd}),
      onItem(onItem),
      ctx(ctx) {
  reset();
}

void CatalogPageParser::reset() {
  parser.reset();
  depth = 0;
  inItems = false;
  inAuthors = false;
  lastKey = Key::NONE;
  failed = false;
  itemAborted = false;
  ended = sawItems = false;
  pageTotal = 0;
  pageHasNext = false;
  items = 0;
  current = CatalogRecord{};
  currentHasId = false;
}

void CatalogPageParser::feed(const char* data, const size_t len) {
  if (itemAborted) return;
  parser.feed(data, len);
}

void CatalogPageParser::commitItem() {
  // Bytes already handed to the tokenizer keep flowing after a stop; drop
  // the items they yield instead of delivering past the callback's refusal.
  if (itemAborted) {
    current = CatalogRecord{};
    currentHasId = false;
    return;
  }
  if (!currentHasId) {
    failed = true;
    return;
  }
  if (current.titleLen == 0) {
    // Never show an empty row; the id keeps the book reachable.
    char fallback[24];
    const int n = snprintf(fallback, sizeof(fallback), "#%lu", static_cast<unsigned long>(current.bookId));
    current.titleLen =
        copyUtf8Bounded(std::string_view(fallback, static_cast<size_t>(n)), current.title, CATALOG_TITLE_BYTES);
  }
  storeFold(current.titleView(), current.fold);
  // surnameKey() already folds, so it goes straight into the prefix slot.
  copyUtf8Bounded(library::surnameKey(current.authorView()), current.authorFold, CATALOG_FOLD_BYTES);
  items++;
  if (onItem && !onItem(ctx, current)) itemAborted = true;
  current = CatalogRecord{};
  currentHasId = false;
}

void CatalogPageParser::appendAuthor(const char* value, const size_t len) {
  std::string text(value, len);
  text.resize(decodeJsonUnicodeEscapes(text.data(), text.size()));
  std::string joined(current.authorView());
  if (!joined.empty()) joined += ", ";
  joined += text;
  current.authorLen = copyUtf8Bounded(joined, current.author, CATALOG_AUTHOR_BYTES);
}

void CatalogPageParser::sOnKey(void* ctx, const char* key, const size_t len) {
  auto* self = static_cast<CatalogPageParser*>(ctx);
  self->lastKey = Key::NONE;
  if (self->depth == 1 && !self->inItems) {
    if (keyIs(key, len, "items"))
      self->lastKey = Key::ITEMS;
    else if (keyIs(key, len, "total"))
      self->lastKey = Key::TOTAL;
    else if (keyIs(key, len, "hasNext"))
      self->lastKey = Key::HAS_NEXT;
  } else if (self->inItem()) {
    if (keyIs(key, len, "id"))
      self->lastKey = Key::ID;
    else if (keyIs(key, len, "title"))
      self->lastKey = Key::TITLE;
    else if (keyIs(key, len, "authors"))
      self->lastKey = Key::AUTHORS;
    else if (keyIs(key, len, "hasCover"))
      self->lastKey = Key::HAS_COVER;
    else if (keyIs(key, len, "progressPercentage"))
      self->lastKey = Key::PROGRESS;
    else if (keyIs(key, len, "lastReadAt"))
      self->lastKey = Key::LAST_READ;
    else if (keyIs(key, len, "addedAt"))
      self->lastKey = Key::ADDED_AT;
    else if (keyIs(key, len, "seriesName"))
      self->lastKey = Key::SERIES_NAME;
    else if (keyIs(key, len, "seriesIndex"))
      self->lastKey = Key::SERIES_INDEX;
  }
}

void CatalogPageParser::sOnString(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<CatalogPageParser*>(ctx);
  if (self->inAuthors) {
    self->appendAuthor(value, len);
    return;
  }
  if (!self->inItem()) {
    self->lastKey = Key::NONE;
    return;
  }
  switch (self->lastKey) {
    case Key::TITLE: {
      std::string text(value, len);
      text.resize(decodeJsonUnicodeEscapes(text.data(), text.size()));
      self->current.titleLen = copyUtf8Bounded(text, self->current.title, CATALOG_TITLE_BYTES);
      break;
    }
    case Key::SERIES_INDEX:
      self->current.seriesIndexTenths = parseSeriesPosition(value, len);
      break;
    case Key::LAST_READ:
      self->current.lastReadAt = parseIso8601Utc(std::string_view(value, len));
      break;
    case Key::ADDED_AT:
      self->current.addedAt = parseIso8601Utc(std::string_view(value, len));
      break;
    case Key::SERIES_NAME: {
      std::string text(value, len);
      text.resize(decodeJsonUnicodeEscapes(text.data(), text.size()));
      self->current.seriesLen = copyUtf8Bounded(text, self->current.series, CATALOG_SERIES_BYTES);
      break;
    }
    default:
      break;
  }
  self->lastKey = Key::NONE;
}

void CatalogPageParser::sOnNumber(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<CatalogPageParser*>(ctx);
  if (self->depth == 1 && !self->inItems && self->lastKey == Key::TOTAL) {
    self->pageTotal = static_cast<uint32_t>(strtoul(value, nullptr, 10));
  } else if (self->inItem()) {
    switch (self->lastKey) {
      case Key::ID:
        self->current.bookId = static_cast<uint32_t>(strtoul(value, nullptr, 10));
        self->currentHasId = self->current.bookId > 0;
        break;
      case Key::PROGRESS: {
        const double pct = strtod(value, nullptr);
        self->current.progressPct = pct < 0 ? 0 : pct > 100 ? 100 : static_cast<uint8_t>(pct + 0.5);
        break;
      }
      case Key::SERIES_INDEX: {
        self->current.seriesIndexTenths = parseSeriesPosition(value, len);
        break;
      }
      default:
        break;
    }
  }
  self->lastKey = Key::NONE;
}

void CatalogPageParser::sOnBool(void* ctx, const bool value) {
  auto* self = static_cast<CatalogPageParser*>(ctx);
  if (self->depth == 1 && !self->inItems && self->lastKey == Key::HAS_NEXT) {
    self->pageHasNext = value;
  } else if (self->inItem() && self->lastKey == Key::HAS_COVER) {
    if (value) self->current.flags |= CATALOG_FLAG_HAS_COVER;
  }
  self->lastKey = Key::NONE;
}

void CatalogPageParser::sOnNull(void* ctx) {
  auto* self = static_cast<CatalogPageParser*>(ctx);
  // null progress / lastReadAt keep the record defaults (none / never).
  self->lastKey = Key::NONE;
}

void CatalogPageParser::sOnObjectStart(void* ctx) {
  auto* self = static_cast<CatalogPageParser*>(ctx);
  self->depth++;
  if (self->inItems && self->depth == 2) {
    self->current = CatalogRecord{};
    self->currentHasId = false;
  }
  self->lastKey = Key::NONE;
}

void CatalogPageParser::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<CatalogPageParser*>(ctx);
  if (self->inItem()) self->commitItem();
  if (self->depth == 1) self->ended = true;
  if (self->depth > 0) self->depth--;
  self->lastKey = Key::NONE;
}

void CatalogPageParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<CatalogPageParser*>(ctx);
  if (self->depth == 1 && !self->inItems && self->lastKey == Key::ITEMS) {
    self->inItems = true;
    self->sawItems = true;
  } else if (self->inItem() && self->lastKey == Key::AUTHORS) {
    self->inAuthors = true;
  }
  self->lastKey = Key::NONE;
}

void CatalogPageParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<CatalogPageParser*>(ctx);
  if (self->inAuthors) {
    self->inAuthors = false;
  } else if (self->inItems && self->depth == 1) {
    self->inItems = false;
  }
  self->lastKey = Key::NONE;
}

}  // namespace bookorbit
