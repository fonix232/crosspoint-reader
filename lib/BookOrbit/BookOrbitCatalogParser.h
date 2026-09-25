#pragma once

#include <cstddef>
#include <cstdint>

#include "BookOrbitCatalogFormat.h"
#include "StreamingJsonParser.h"

namespace bookorbit {

// Streaming parser for `GET /koreader/plugin/catalog/books` responses.
//
// Feeds the response body in as it arrives and emits one CatalogRecord per
// item through `onItem`, so no catalogue page is ever buffered whole. Only the
// fields the Library shows are kept; everything else is skipped at the token
// level. Page metadata (`total`, `hasNext`) is available after the body ends.
class CatalogPageParser {
 public:
  using ItemCallback = bool (*)(void* ctx, const CatalogRecord& record);

  CatalogPageParser(ItemCallback onItem, void* ctx);
  CatalogPageParser(const CatalogPageParser&) = delete;
  CatalogPageParser& operator=(const CatalogPageParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  // Assemble one logical page from multiple library-filtered server pages.
  bool emitRecord(const CatalogRecord& record) {
    if (itemAborted) return false;
    ++items;
    if (onItem && !onItem(ctx, record)) itemAborted = true;
    return !itemAborted;
  }
  void setPageMetadata(uint32_t total, bool next) {
    pageTotal = total;
    pageHasNext = next;
    ended = sawItems = true;
  }

  bool hasError() const { return parser.hasError() || failed; }
  bool aborted() const { return itemAborted; }
  bool complete() const { return ended && sawItems && depth == 0 && !inItems && !hasError() && !aborted(); }
  uint32_t total() const { return pageTotal; }
  bool hasNext() const { return pageHasNext; }
  uint16_t itemCount() const { return items; }

 private:
  enum class Key : uint8_t {
    NONE,
    ITEMS,
    TOTAL,
    HAS_NEXT,
    ID,
    TITLE,
    AUTHORS,
    HAS_COVER,
    PROGRESS,
    LAST_READ,
    ADDED_AT,
    SERIES_NAME,
    SERIES_INDEX,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  bool inItem() const { return inItems && depth == 2; }
  void commitItem();
  void appendAuthor(const char* value, size_t len);

  StreamingJsonParser parser;
  ItemCallback onItem;
  void* ctx;

  uint8_t depth = 0;  // object nesting depth
  bool inItems = false;
  bool inAuthors = false;
  Key lastKey = Key::NONE;
  bool failed = false;
  bool itemAborted = false;

  uint32_t pageTotal = 0;
  bool pageHasNext = false;
  uint16_t items = 0;

  CatalogRecord current;
  bool currentHasId = false;
  bool ended = false;
  bool sawItems = false;
};

// Decodes JSON \uXXXX escapes (including surrogate pairs) that the streaming
// parser passes through verbatim, in place. Other bytes are copied as-is.
size_t decodeJsonUnicodeEscapes(char* text, size_t len);

}  // namespace bookorbit
