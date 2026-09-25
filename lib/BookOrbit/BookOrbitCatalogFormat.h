#pragma once

// On-card format of the BookOrbit catalogue cache (`catalog.bin`).
//
// A fixed 32-byte header followed by fixed 264-byte records in the server's
// title order, so record k is always at 32 + 264k and a page of rows is a
// handful of seeks. Two optional sidecar index files hold uint16_t ordinals
// for the author and recent orders. Nothing proportional to the catalogue is
// ever held in RAM by the reader side.
//
// All multi-byte fields are little-endian and copied with memcpy: the record
// is never cast in place (RISC-V faults on unaligned loads).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace bookorbit {

inline constexpr uint32_t CATALOG_MAGIC = 0x54434F42;  // "BOCT"
inline constexpr uint8_t CATALOG_VERSION = 2;          // v2: series name and index
// v1 records lacked the series tail; a v1 file is upgraded in place on open.
inline constexpr uint8_t CATALOG_VERSION_V1 = 1;
inline constexpr size_t CATALOG_RECORD_BYTES_V1 = 208;
inline constexpr size_t CATALOG_HEADER_BYTES = 32;
inline constexpr size_t CATALOG_RECORD_BYTES = 264;
inline constexpr size_t CATALOG_TITLE_BYTES = 96;
inline constexpr size_t CATALOG_AUTHOR_BYTES = 48;
inline constexpr size_t CATALOG_FOLD_BYTES = 24;
inline constexpr size_t CATALOG_SERIES_BYTES = 48;
// Upper bound on cached books. The Library shows this many of the server's
// title order; the header's serverTotal says how many exist.
inline constexpr uint16_t CATALOG_MAX_BOOKS = 1000;

inline constexpr uint8_t CATALOG_FLAG_HAS_COVER = 1u << 0;
inline constexpr uint8_t CATALOG_PROGRESS_NONE = 255;

struct CatalogHeader {
  uint16_t count = 0;
  uint32_t serverTotal = 0;
  uint32_t fetchedAt = 0;  // Unix seconds, 0 when the clock was invalid
  bool truncated = false;  // serverTotal exceeded CATALOG_MAX_BOOKS
};

struct CatalogRecord {
  uint32_t bookId = 0;
  uint32_t addedAt = 0;     // Unix seconds, 0 unknown
  uint32_t lastReadAt = 0;  // Unix seconds, 0 never
  uint8_t flags = 0;
  uint8_t progressPct = CATALOG_PROGRESS_NONE;  // 0..100
  uint8_t titleLen = 0;
  uint8_t authorLen = 0;
  uint8_t seriesLen = 0;
  uint16_t seriesIndexTenths = 0;  // series position x10 (3 -> 30, 1.5 -> 15); 0 = none
  char title[CATALOG_TITLE_BYTES] = {};
  char author[CATALOG_AUTHOR_BYTES] = {};
  char fold[CATALOG_FOLD_BYTES] = {};        // folded, article-stripped title prefix
  char authorFold[CATALOG_FOLD_BYTES] = {};  // folded author prefix
  char series[CATALOG_SERIES_BYTES] = {};

  std::string_view titleView() const { return std::string_view(title, titleLen); }
  std::string_view authorView() const { return std::string_view(author, authorLen); }
  std::string_view seriesView() const { return std::string_view(series, seriesLen); }
  std::string_view foldView() const { return std::string_view(fold, strnlen(fold, CATALOG_FOLD_BYTES)); }
  std::string_view authorFoldView() const {
    return std::string_view(authorFold, strnlen(authorFold, CATALOG_FOLD_BYTES));
  }
  bool hasCover() const { return (flags & CATALOG_FLAG_HAS_COVER) != 0; }
};

void encodeHeader(const CatalogHeader& header, uint8_t out[CATALOG_HEADER_BYTES]);
bool decodeHeader(const uint8_t in[CATALOG_HEADER_BYTES], CatalogHeader& out);
// The version byte of a header that carries the magic, 0 otherwise.
uint8_t headerVersion(const uint8_t in[CATALOG_HEADER_BYTES]);
void encodeRecord(const CatalogRecord& record, uint8_t out[CATALOG_RECORD_BYTES]);
void decodeRecord(const uint8_t in[CATALOG_RECORD_BYTES], CatalogRecord& out);

// Copies at most `capacity` bytes of UTF-8 without splitting a codepoint.
// Returns the stored length.
uint8_t copyUtf8Bounded(std::string_view text, char* out, size_t capacity);

// "2025-01-02T03:04:05.000Z" (or without fraction / with +00:00) to Unix
// seconds; 0 when the text is not a UTC ISO-8601 timestamp.
uint32_t parseIso8601Utc(std::string_view text);

inline size_t recordOffset(const uint16_t ordinal) {
  return CATALOG_HEADER_BYTES + static_cast<size_t>(ordinal) * CATALOG_RECORD_BYTES;
}

}  // namespace bookorbit
