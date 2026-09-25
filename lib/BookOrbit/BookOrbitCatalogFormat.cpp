#include "BookOrbitCatalogFormat.h"

#include <Utf8.h>

#include <algorithm>

namespace bookorbit {

namespace {
void put32(uint8_t* out, const uint32_t v) {
  out[0] = static_cast<uint8_t>(v);
  out[1] = static_cast<uint8_t>(v >> 8);
  out[2] = static_cast<uint8_t>(v >> 16);
  out[3] = static_cast<uint8_t>(v >> 24);
}
void put16(uint8_t* out, const uint16_t v) {
  out[0] = static_cast<uint8_t>(v);
  out[1] = static_cast<uint8_t>(v >> 8);
}
uint32_t get32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}
uint16_t get16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | (in[1] << 8)); }

bool readDigits(std::string_view text, size_t& pos, const size_t count, uint32_t& out) {
  if (pos + count > text.size()) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < count; i++) {
    const char c = text[pos + i];
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<uint32_t>(c - '0');
  }
  pos += count;
  out = value;
  return true;
}

// Howard Hinnant's days-from-civil, valid for the Unix range.
int64_t daysFromCivil(int64_t y, const uint32_t m, const uint32_t d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
}  // namespace

void encodeHeader(const CatalogHeader& header, uint8_t out[CATALOG_HEADER_BYTES]) {
  memset(out, 0, CATALOG_HEADER_BYTES);
  put32(out, CATALOG_MAGIC);
  out[4] = CATALOG_VERSION;
  out[5] = header.truncated ? 1 : 0;
  put16(out + 6, header.count);
  put32(out + 8, header.serverTotal);
  put32(out + 12, header.fetchedAt);
}

uint8_t headerVersion(const uint8_t in[CATALOG_HEADER_BYTES]) { return get32(in) == CATALOG_MAGIC ? in[4] : 0; }

bool decodeHeader(const uint8_t in[CATALOG_HEADER_BYTES], CatalogHeader& out) {
  if (get32(in) != CATALOG_MAGIC || in[4] != CATALOG_VERSION) return false;
  out.truncated = in[5] != 0;
  out.count = get16(in + 6);
  out.serverTotal = get32(in + 8);
  out.fetchedAt = get32(in + 12);
  return out.count <= CATALOG_MAX_BOOKS;
}

void encodeRecord(const CatalogRecord& record, uint8_t out[CATALOG_RECORD_BYTES]) {
  memset(out, 0, CATALOG_RECORD_BYTES);
  put32(out, record.bookId);
  put32(out + 4, record.addedAt);
  put32(out + 8, record.lastReadAt);
  out[12] = record.flags;
  out[13] = record.progressPct;
  out[14] = record.titleLen;
  out[15] = record.authorLen;
  memcpy(out + 16, record.title, CATALOG_TITLE_BYTES);
  memcpy(out + 16 + CATALOG_TITLE_BYTES, record.author, CATALOG_AUTHOR_BYTES);
  memcpy(out + 16 + CATALOG_TITLE_BYTES + CATALOG_AUTHOR_BYTES, record.fold, CATALOG_FOLD_BYTES);
  memcpy(out + 16 + CATALOG_TITLE_BYTES + CATALOG_AUTHOR_BYTES + CATALOG_FOLD_BYTES, record.authorFold,
         CATALOG_FOLD_BYTES);
  // v2 tail: series (offset 208).
  constexpr size_t SERIES_OFFSET = 16 + CATALOG_TITLE_BYTES + CATALOG_AUTHOR_BYTES + CATALOG_FOLD_BYTES * 2;
  out[SERIES_OFFSET] = record.seriesLen;
  put16(out + SERIES_OFFSET + 1, record.seriesIndexTenths);
  memcpy(out + SERIES_OFFSET + 4, record.series, CATALOG_SERIES_BYTES);
}

void decodeRecord(const uint8_t in[CATALOG_RECORD_BYTES], CatalogRecord& out) {
  out.bookId = get32(in);
  out.addedAt = get32(in + 4);
  out.lastReadAt = get32(in + 8);
  out.flags = in[12];
  out.progressPct = in[13];
  out.titleLen = std::min<uint8_t>(in[14], CATALOG_TITLE_BYTES);
  out.authorLen = std::min<uint8_t>(in[15], CATALOG_AUTHOR_BYTES);
  memcpy(out.title, in + 16, CATALOG_TITLE_BYTES);
  memcpy(out.author, in + 16 + CATALOG_TITLE_BYTES, CATALOG_AUTHOR_BYTES);
  memcpy(out.fold, in + 16 + CATALOG_TITLE_BYTES + CATALOG_AUTHOR_BYTES, CATALOG_FOLD_BYTES);
  memcpy(out.authorFold, in + 16 + CATALOG_TITLE_BYTES + CATALOG_AUTHOR_BYTES + CATALOG_FOLD_BYTES, CATALOG_FOLD_BYTES);
  constexpr size_t SERIES_OFFSET = 16 + CATALOG_TITLE_BYTES + CATALOG_AUTHOR_BYTES + CATALOG_FOLD_BYTES * 2;
  out.seriesLen = std::min<uint8_t>(in[SERIES_OFFSET], CATALOG_SERIES_BYTES);
  out.seriesIndexTenths = get16(in + SERIES_OFFSET + 1);
  memcpy(out.series, in + SERIES_OFFSET + 4, CATALOG_SERIES_BYTES);
}

uint8_t copyUtf8Bounded(std::string_view text, char* out, const size_t capacity) {
  const size_t want = std::min(text.size(), capacity);
  const int len = utf8SafeTruncateBuffer(text.data(), static_cast<int>(want));
  memcpy(out, text.data(), static_cast<size_t>(len));
  if (static_cast<size_t>(len) < capacity) memset(out + len, 0, capacity - static_cast<size_t>(len));
  return static_cast<uint8_t>(len);
}

uint32_t parseIso8601Utc(std::string_view text) {
  size_t pos = 0;
  uint32_t year, month, day, hour = 0, minute = 0, second = 0;
  if (!readDigits(text, pos, 4, year)) return 0;
  if (pos >= text.size() || text[pos++] != '-') return 0;
  if (!readDigits(text, pos, 2, month)) return 0;
  if (pos >= text.size() || text[pos++] != '-') return 0;
  if (!readDigits(text, pos, 2, day)) return 0;
  if (month < 1 || month > 12 || day < 1 || day > 31) return 0;
  if (pos < text.size() && (text[pos] == 'T' || text[pos] == ' ')) {
    pos++;
    if (!readDigits(text, pos, 2, hour)) return 0;
    if (pos >= text.size() || text[pos++] != ':') return 0;
    if (!readDigits(text, pos, 2, minute)) return 0;
    if (pos < text.size() && text[pos] == ':') {
      pos++;
      if (!readDigits(text, pos, 2, second)) return 0;
    }
    if (pos < text.size() && text[pos] == '.') {
      pos++;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') pos++;
    }
    // Only UTC forms are accepted; an offset would need a timezone table.
    if (pos < text.size()) {
      const std::string_view rest = text.substr(pos);
      if (rest != "Z" && rest != "+00:00" && rest != "+0000") return 0;
    }
  } else if (pos != text.size()) {
    return 0;
  }
  if (hour > 23 || minute > 59 || second > 60) return 0;
  const int64_t days = daysFromCivil(static_cast<int64_t>(year), month, day);
  const int64_t seconds = days * 86400 + static_cast<int64_t>(hour) * 3600 + minute * 60 + second;
  if (seconds <= 0 || seconds > 0xFFFFFFFFll) return 0;
  return static_cast<uint32_t>(seconds);
}

}  // namespace bookorbit
