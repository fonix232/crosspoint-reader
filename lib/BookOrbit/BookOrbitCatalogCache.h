#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "BookOrbitCatalogFormat.h"

namespace bookorbit {

// Series order puts every book that belongs to a series first, grouped by
// series (folded name) and ordered by its index within the series; the
// rest follow in title order. seriesRowCount() says where the groups end.
enum class CatalogOrder : uint8_t { Title, Author, Recent, Series };

// Read side of the catalogue cache. Holds one open file handle and, while a
// non-title order is active, one 2-byte-per-book ordinal array (at most 2 KiB).
class CatalogCache {
 public:
  CatalogCache() = default;
  ~CatalogCache();
  CatalogCache(const CatalogCache&) = delete;
  CatalogCache& operator=(const CatalogCache&) = delete;

  bool open(const std::string& accountKey);
  void close();
  bool isOpen() const { return opened; }
  const CatalogHeader& header() const { return head; }
  uint16_t count() const { return opened ? head.count : 0; }

  // Loads the ordinal array for `order` (title order needs none). Returns
  // false when the sidecar index is missing or stale; rows then fall back to
  // title order and the caller can say so.
  bool selectOrder(CatalogOrder order);
  bool orderDegraded() const { return degraded; }
  // Rows that belong to a series while the Series order is active, else 0.
  uint16_t seriesRowCount() const { return order == CatalogOrder::Series && ordinals ? seriesRows : 0; }

  // Row (ascending in the active order) to record ordinal; 0xFFFF when out
  // of range.
  uint16_t ordinalForRow(uint16_t row) const;
  bool readRecord(uint16_t ordinal, CatalogRecord& out);
  // Linear scan; only for one-off lookups such as a download continuation.
  uint16_t ordinalForBook(uint32_t bookId);

  // Rows whose folded title or author match `needle` (see library::matchesQuery),
  // written as ascending rows of the active order. `out` must hold count()
  // entries. Returns the number of matches.
  uint16_t filterRows(std::string_view needle, uint16_t* out);

 private:
  bool loadIndex(const std::string& path, uint16_t* seriesRowsOut = nullptr);
  bool upgradeV1(const std::string& path);

  HalFile file;
  bool opened = false;
  std::string account;
  CatalogHeader head;
  CatalogOrder order = CatalogOrder::Title;
  bool degraded = false;
  std::unique_ptr<uint16_t[]> ordinals;  // active non-title order
  uint16_t seriesRows = 0;
};

// Write side, used by the sync session. Records go to a staging file in the
// order received; commit() finalizes the header, swaps the live file in, and
// rebuilds the sidecar indices.
class CatalogWriter {
 public:
  CatalogWriter() = default;
  ~CatalogWriter();
  CatalogWriter(const CatalogWriter&) = delete;
  CatalogWriter& operator=(const CatalogWriter&) = delete;

  bool begin(const std::string& accountKey);
  bool append(const CatalogRecord& record);
  uint16_t count() const { return written; }
  // Finalizes and installs the staged catalogue. The old catalogue stays in
  // place until the new one is complete on the card.
  bool sortByTitle();
  bool commit(uint32_t serverTotal, bool truncated, uint32_t fetchedAt);
  void abort();

 private:
  HalFile file;
  bool active = false;
  std::string account;
  uint16_t written = 0;
  std::string scopeSelection;
};

// Rebuilds catalog.author.idx and catalog.recent.idx from the live catalogue.
// Each index needs a fallible key array proportional to the catalogue (at
// most ~26 KiB for the author keys); a failed allocation leaves that index
// absent, which the reader reports as a degraded order rather than an error.
bool buildCatalogIndices(const std::string& accountKey);

}  // namespace bookorbit
