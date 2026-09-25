#include "BookOrbitCatalogCache.h"

#include <BookOrbitCredentialStore.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <PersistableStore.h>

#include <algorithm>
#include <cstring>

#include "BookOrbitPaths.h"

namespace bookorbit {

namespace {
constexpr char TAG[] = "BOCAT";

// Sidecar index header: magic, catalogue fetchedAt, count, and (series index
// only) how many leading rows belong to a series. A stale index from a
// previous catalogue generation fails validation and is ignored.
constexpr uint32_t INDEX_MAGIC = 0x58444F42;  // "BODX"
constexpr size_t INDEX_HEADER_BYTES = 16;

void put32(uint8_t* out, const uint32_t v) {
  out[0] = static_cast<uint8_t>(v);
  out[1] = static_cast<uint8_t>(v >> 8);
  out[2] = static_cast<uint8_t>(v >> 16);
  out[3] = static_cast<uint8_t>(v >> 24);
}
uint32_t get32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}
uint16_t get16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | (in[1] << 8)); }

bool readHeaderFrom(HalFile& file, CatalogHeader& out) {
  uint8_t raw[CATALOG_HEADER_BYTES];
  if (!file.seekSet(0) || file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return false;
  return decodeHeader(raw, out);
}

bool writeIndexFile(const std::string& path, const CatalogHeader& head, const uint16_t* ordinals,
                    const uint16_t seriesRows = 0) {
  const std::string tmp = path + ".tmp";
  {
    HalFile out;
    if (!Storage.openFileForWrite(TAG, tmp.c_str(), out)) return false;
    uint8_t header[INDEX_HEADER_BYTES];
    put32(header, INDEX_MAGIC);
    put32(header + 4, head.fetchedAt);
    put32(header + 8, head.count);
    put32(header + 12, seriesRows);
    if (out.write(header, sizeof(header)) != sizeof(header)) return false;
    uint8_t chunk[128];
    size_t pending = 0;
    for (uint16_t i = 0; i < head.count; i++) {
      chunk[pending++] = static_cast<uint8_t>(ordinals[i]);
      chunk[pending++] = static_cast<uint8_t>(ordinals[i] >> 8);
      if (pending == sizeof(chunk)) {
        if (out.write(chunk, pending) != pending) return false;
        pending = 0;
      }
    }
    if (pending > 0 && out.write(chunk, pending) != pending) return false;
    out.flush();
  }
  Storage.remove(path.c_str());
  return Storage.rename(tmp.c_str(), path.c_str());
}

// Author keys are the folded surname-first prefix; ties keep title order.
struct AuthorKey {
  char key[CATALOG_FOLD_BYTES];
  uint16_t ordinal;
};

struct RecentKey {
  uint32_t addedAt;
  uint16_t ordinal;
};

struct SeriesKey {
  char key[CATALOG_FOLD_BYTES];
  uint16_t indexTenths;
  uint16_t ordinal;
  bool inSeries;
};
}  // namespace

CatalogCache::~CatalogCache() { close(); }

// A v1 file (no series tail) becomes v2 with empty series fields, written
// through the staging path so an interruption leaves the old file intact. The
// sidecar indices stay valid: they hold ordinals keyed by fetchedAt and count.
bool CatalogCache::upgradeV1(const std::string& path) {
  HalFile in;
  if (!Storage.openFileForRead(TAG, path.c_str(), in)) return false;
  uint8_t header[CATALOG_HEADER_BYTES];
  if (in.read(header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
      headerVersion(header) != CATALOG_VERSION_V1) {
    in.close();
    return false;
  }
  const uint16_t count = get16(header + 6);
  header[4] = CATALOG_VERSION;
  const std::string staging = catalogStagingPath(account);
  Storage.remove(staging.c_str());
  HalFile out;
  if (!Storage.openFileForWrite(TAG, staging.c_str(), out)) {
    in.close();
    return false;
  }
  bool ok = out.write(header, sizeof(header)) == sizeof(header);
  uint8_t record[CATALOG_RECORD_BYTES];
  for (uint16_t i = 0; ok && i < count; i++) {
    memset(record, 0, sizeof(record));
    ok = in.read(record, CATALOG_RECORD_BYTES_V1) == static_cast<int>(CATALOG_RECORD_BYTES_V1) &&
         out.write(record, sizeof(record)) == sizeof(record);
  }
  out.close();
  in.close();
  if (!ok) {
    Storage.remove(staging.c_str());
    return false;
  }
  Storage.remove(path.c_str());
  if (!Storage.rename(staging.c_str(), path.c_str())) return false;
  LOG_INF(TAG, "catalogue upgraded to v%u (%u books)", static_cast<unsigned>(CATALOG_VERSION),
          static_cast<unsigned>(count));
  return true;
}

bool CatalogCache::open(const std::string& accountKey) {
  close();
  account = accountKey;
  JsonDocument scope;
  const auto scopePath = accountDir(account) + "/catalog-scope.json";
  const bool hasScope = Storage.exists(scopePath.c_str());
  if (hasScope && !PersistableStoreBase::readDocFromFile(scopePath.c_str(), scope)) return false;
  if (BOOKORBIT_STORE.getLibraries().key() != (scope["selection"] | "all")) return false;
  const std::string path = catalogPath(accountKey);
  if (!Storage.exists(path.c_str())) return false;  // nothing synced yet
  if (!Storage.openFileForRead(TAG, path.c_str(), file)) return false;
  if (!readHeaderFrom(file, head)) {
    uint8_t raw[CATALOG_HEADER_BYTES];
    const bool oldVersion = file.seekSet(0) && file.read(raw, sizeof(raw)) == static_cast<int>(sizeof(raw)) &&
                            headerVersion(raw) == CATALOG_VERSION_V1;
    file.close();
    // A cache from an older firmware is upgraded rather than dropped, so an
    // update never empties the Library until the next sync.
    if (!oldVersion || !upgradeV1(path) || !Storage.openFileForRead(TAG, path.c_str(), file) ||
        !readHeaderFrom(file, head)) {
      LOG_ERR(TAG, "invalid catalogue header: %s", path.c_str());
      if (file) file.close();
      return false;
    }
  }
  opened = true;
  order = CatalogOrder::Title;
  degraded = false;
  return true;
}

void CatalogCache::close() {
  if (opened) file.close();
  opened = false;
  ordinals.reset();
  head = CatalogHeader{};
}

bool CatalogCache::loadIndex(const std::string& path, uint16_t* seriesRowsOut) {
  ordinals.reset();
  HalFile idx;
  if (!Storage.exists(path.c_str()) || !Storage.openFileForRead(TAG, path.c_str(), idx)) return false;
  uint8_t header[INDEX_HEADER_BYTES];
  if (idx.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return false;
  if (get32(header) != INDEX_MAGIC || get32(header + 4) != head.fetchedAt || get32(header + 8) != head.count) {
    LOG_DBG(TAG, "stale sidecar index: %s", path.c_str());
    return false;
  }
  if (seriesRowsOut) {
    const uint32_t rows = get32(header + 12);
    if (rows > head.count) return false;
    *seriesRowsOut = static_cast<uint16_t>(rows);
  }
  if (head.count == 0) return true;
  auto array = makeUniqueNoThrow<uint16_t[]>(head.count);
  if (!array) {
    LOG_ERR(TAG, "OOM: %u-byte order index", static_cast<unsigned>(head.count * 2));
    return false;
  }
  const size_t bytes = static_cast<size_t>(head.count) * 2;
  if (idx.read(array.get(), bytes) != static_cast<int>(bytes)) return false;
  // Stored little-endian; on a little-endian target this is already the
  // in-memory layout, but validate every ordinal regardless.
  for (uint16_t i = 0; i < head.count; i++) {
    const auto* raw = reinterpret_cast<const uint8_t*>(array.get()) + i * 2;
    const uint16_t value = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
    if (value >= head.count) return false;
    array[i] = value;
  }
  ordinals = std::move(array);
  return true;
}

bool CatalogCache::selectOrder(const CatalogOrder wanted) {
  if (!opened) return false;
  order = wanted;
  degraded = false;
  if (wanted == CatalogOrder::Title) {
    ordinals.reset();
    return true;
  }
  seriesRows = 0;
  std::string path;
  bool ok;
  switch (wanted) {
    case CatalogOrder::Author:
      ok = loadIndex(catalogAuthorIndexPath(account));
      break;
    case CatalogOrder::Series:
      ok = loadIndex(catalogSeriesIndexPath(account), &seriesRows);
      break;
    default:
      ok = loadIndex(catalogRecentIndexPath(account));
      break;
  }
  if (!ok) {
    degraded = true;
    return false;
  }
  return true;
}

uint16_t CatalogCache::ordinalForRow(const uint16_t row) const {
  if (!opened || row >= head.count) return 0xFFFF;
  if (order == CatalogOrder::Title || !ordinals) return row;
  return ordinals[row];
}

bool CatalogCache::readRecord(const uint16_t ordinal, CatalogRecord& out) {
  if (!opened || ordinal >= head.count) return false;
  uint8_t raw[CATALOG_RECORD_BYTES];
  if (!file.seekSet(recordOffset(ordinal)) || file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) {
    LOG_ERR(TAG, "record %u read failed", ordinal);
    return false;
  }
  decodeRecord(raw, out);
  return true;
}

uint16_t CatalogCache::ordinalForBook(const uint32_t bookId) {
  CatalogRecord record;
  for (uint16_t i = 0; i < count(); i++) {
    if (readRecord(i, record) && record.bookId == bookId) return i;
  }
  return 0xFFFF;
}

uint16_t CatalogCache::filterRows(const std::string_view needle, uint16_t* out) {
  uint16_t matches = 0;
  CatalogRecord record;
  for (uint16_t row = 0; row < count(); row++) {
    const uint16_t ordinal = ordinalForRow(row);
    if (ordinal == 0xFFFF || !readRecord(ordinal, record)) continue;
    if (library::matchesQuery(record.foldView(), needle) || library::matchesQuery(record.authorFoldView(), needle)) {
      out[matches++] = row;
    }
  }
  return matches;
}

// --- writer ------------------------------------------------------------------

CatalogWriter::~CatalogWriter() { abort(); }

bool CatalogWriter::begin(const std::string& accountKey) {
  abort();
  scopeSelection = BOOKORBIT_STORE.getLibraries().key();
  account = accountKey;
  Storage.mkdir(accountDir(accountKey).c_str());
  const std::string tmp = catalogStagingPath(accountKey);
  Storage.remove(tmp.c_str());
  if (!Storage.openFileForWrite(TAG, tmp.c_str(), file)) {
    LOG_ERR(TAG, "cannot open staging catalogue");
    return false;
  }
  uint8_t header[CATALOG_HEADER_BYTES];
  encodeHeader(CatalogHeader{}, header);
  if (file.write(header, sizeof(header)) != sizeof(header)) {
    file.close();
    return false;
  }
  active = true;
  written = 0;
  return true;
}

bool CatalogWriter::append(const CatalogRecord& record) {
  if (!active || written >= CATALOG_MAX_BOOKS) return false;
  uint8_t raw[CATALOG_RECORD_BYTES];
  encodeRecord(record, raw);
  if (file.write(raw, sizeof(raw)) != sizeof(raw)) {
    LOG_ERR(TAG, "staging write failed at record %u", written);
    return false;
  }
  written++;
  return true;
}

// Multi-library requests arrive in library order. Sort only compact keys
// (26 KiB at the 1000-book cap), then stream records to a second staging file.
// No page/catalogue-sized record buffer is kept in RAM.
bool CatalogWriter::sortByTitle() {
  if (!active || written < 2) return active;
  auto keys = makeUniqueNoThrow<AuthorKey[]>(written);
  if (!keys) return false;
  file.flush();
  file.close();
  const auto source = catalogStagingPath(account);
  HalFile input, output;
  if (!Storage.openFileForRead(TAG, source.c_str(), input)) return false;
  uint8_t chunk[128];
  constexpr size_t foldOffset = 16 + CATALOG_TITLE_BYTES + CATALOG_AUTHOR_BYTES;
  for (uint16_t i = 0; i < written; ++i) {
    if (!input.seekSet(CATALOG_HEADER_BYTES + static_cast<uint32_t>(i) * CATALOG_RECORD_BYTES + foldOffset) ||
        input.read(keys[i].key, CATALOG_FOLD_BYTES) != CATALOG_FOLD_BYTES)
      return false;
    keys[i].ordinal = i;
  }
  std::sort(keys.get(), keys.get() + written, [](const AuthorKey& a, const AuthorKey& b) {
    const int c = memcmp(a.key, b.key, CATALOG_FOLD_BYTES);
    return c ? c < 0 : a.ordinal < b.ordinal;
  });
  const auto sorted = source + ".sorted";
  if (!Storage.openFileForWrite(TAG, sorted.c_str(), output)) return false;
  uint8_t header[CATALOG_HEADER_BYTES];
  encodeHeader(CatalogHeader{}, header);
  if (output.write(header, sizeof(header)) != sizeof(header)) return false;
  for (uint16_t i = 0; i < written; ++i) {
    if (!input.seekSet(CATALOG_HEADER_BYTES + static_cast<uint32_t>(keys[i].ordinal) * CATALOG_RECORD_BYTES))
      return false;
    for (size_t copied = 0; copied < CATALOG_RECORD_BYTES;) {
      const size_t amount = std::min(sizeof(chunk), CATALOG_RECORD_BYTES - copied);
      if (input.read(chunk, amount) != static_cast<int>(amount) || output.write(chunk, amount) != amount) return false;
      copied += amount;
    }
  }
  output.flush();
  output.close();
  input.close();
  Storage.remove(source.c_str());
  if (!Storage.rename(sorted.c_str(), source.c_str())) return false;
  file = Storage.open(source.c_str(), O_RDWR);
  return static_cast<bool>(file);
}

bool CatalogWriter::commit(const uint32_t serverTotal, const bool truncated, const uint32_t fetchedAt) {
  if (!active) return false;
  CatalogHeader head;
  head.count = written;
  head.serverTotal = serverTotal;
  head.truncated = truncated;
  head.fetchedAt = fetchedAt;
  uint8_t header[CATALOG_HEADER_BYTES];
  encodeHeader(head, header);
  const bool ok = file.seekSet(0) && file.write(header, sizeof(header)) == sizeof(header);
  file.flush();
  if (file) file.close();
  active = false;
  if (!ok) {
    LOG_ERR(TAG, "staging header write failed");
    Storage.remove(catalogStagingPath(account).c_str());
    return false;
  }
  // Sidecars from the previous generation would validate against the old
  // header; drop them before the swap so a crash in between cannot pair a new
  // catalogue with stale orders.
  Storage.remove(catalogAuthorIndexPath(account).c_str());
  Storage.remove(catalogRecentIndexPath(account).c_str());
  Storage.remove(catalogSeriesIndexPath(account).c_str());
  const std::string live = catalogPath(account);
  Storage.remove(live.c_str());
  if (!Storage.rename(catalogStagingPath(account).c_str(), live.c_str())) {
    LOG_ERR(TAG, "cannot install catalogue");
    return false;
  }
  LOG_INF(TAG, "catalogue installed: %u of %lu books", written, static_cast<unsigned long>(serverTotal));
  JsonDocument scope;
  scope["selection"] = scopeSelection;
  if (!PersistableStoreBase::writeDocToFile((accountDir(account) + "/catalog-scope.json").c_str(), scope)) return false;
  if (!buildCatalogIndices(account)) LOG_ERR(TAG, "order indices incomplete; title order only");
  return true;
}

void CatalogWriter::abort() {
  if (!active) return;
  if (file) file.close();
  active = false;
  Storage.remove(catalogStagingPath(account).c_str());
  Storage.remove((catalogStagingPath(account) + ".sorted").c_str());
}

// --- indices -----------------------------------------------------------------

bool buildCatalogIndices(const std::string& accountKey) {
  HalFile file;
  if (!Storage.openFileForRead(TAG, catalogPath(accountKey).c_str(), file)) return false;
  CatalogHeader head;
  if (!readHeaderFrom(file, head)) return false;
  if (head.count == 0) {
    uint16_t none = 0;
    return writeIndexFile(catalogAuthorIndexPath(accountKey), head, &none) &&
           writeIndexFile(catalogRecentIndexPath(accountKey), head, &none) &&
           writeIndexFile(catalogSeriesIndexPath(accountKey), head, &none);
  }

  bool allOk = true;
  uint8_t raw[CATALOG_RECORD_BYTES];
  CatalogRecord record;

  // Author order: one pass collecting keys, sort, write, release.
  {
    auto keys = makeUniqueNoThrow<AuthorKey[]>(head.count);
    auto ordinals = makeUniqueNoThrow<uint16_t[]>(head.count);
    if (!keys || !ordinals) {
      LOG_ERR(TAG, "OOM: author index (%u books)", head.count);
      allOk = false;
    } else {
      bool readOk = true;
      for (uint16_t i = 0; i < head.count && readOk; i++) {
        readOk = file.seekSet(recordOffset(i)) && file.read(raw, sizeof(raw)) == static_cast<int>(sizeof(raw));
        if (!readOk) break;
        decodeRecord(raw, record);
        memcpy(keys[i].key, record.authorFold, CATALOG_FOLD_BYTES);
        keys[i].ordinal = i;
      }
      if (readOk) {
        std::sort(keys.get(), keys.get() + head.count, [](const AuthorKey& a, const AuthorKey& b) {
          const int c = memcmp(a.key, b.key, CATALOG_FOLD_BYTES);
          // Unknown authors sort after named ones; equal names keep title order.
          const bool aEmpty = a.key[0] == '\0';
          const bool bEmpty = b.key[0] == '\0';
          if (aEmpty != bEmpty) return bEmpty;
          return c != 0 ? c < 0 : a.ordinal < b.ordinal;
        });
        for (uint16_t i = 0; i < head.count; i++) ordinals[i] = keys[i].ordinal;
        keys.reset();
        if (!writeIndexFile(catalogAuthorIndexPath(accountKey), head, ordinals.get())) allOk = false;
      } else {
        allOk = false;
      }
    }
  }

  // Recent order: most recently read anywhere, else most recently added,
  // newest first; unknown dates last, ties in title order.
  {
    auto keys = makeUniqueNoThrow<RecentKey[]>(head.count);
    auto ordinals = makeUniqueNoThrow<uint16_t[]>(head.count);
    if (!keys || !ordinals) {
      LOG_ERR(TAG, "OOM: recent index (%u books)", head.count);
      return false;
    }
    for (uint16_t i = 0; i < head.count; i++) {
      if (!file.seekSet(recordOffset(i)) || file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return false;
      decodeRecord(raw, record);
      keys[i].addedAt = record.lastReadAt > record.addedAt ? record.lastReadAt : record.addedAt;
      keys[i].ordinal = i;
    }
    std::sort(keys.get(), keys.get() + head.count, [](const RecentKey& a, const RecentKey& b) {
      if (a.addedAt != b.addedAt) return a.addedAt > b.addedAt;
      return a.ordinal < b.ordinal;
    });
    for (uint16_t i = 0; i < head.count; i++) ordinals[i] = keys[i].ordinal;
    keys.reset();
    if (!writeIndexFile(catalogRecentIndexPath(accountKey), head, ordinals.get())) allOk = false;
  }

  // Series order: books in a series first, by folded series name then index
  // within the series (title order breaks ties); the rest in title order.
  {
    auto keys = makeUniqueNoThrow<SeriesKey[]>(head.count);
    auto ordinals = makeUniqueNoThrow<uint16_t[]>(head.count);
    if (!keys || !ordinals) {
      LOG_ERR(TAG, "OOM: series index (%u books)", head.count);
      return false;
    }
    uint16_t inSeries = 0;
    for (uint16_t i = 0; i < head.count; i++) {
      if (!file.seekSet(recordOffset(i)) || file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return false;
      decodeRecord(raw, record);
      memset(keys[i].key, 0, sizeof(keys[i].key));
      keys[i].inSeries = record.seriesLen > 0;
      if (keys[i].inSeries) {
        const std::string folded = library::fold(std::string(record.seriesView()));
        copyUtf8Bounded(folded, keys[i].key, CATALOG_FOLD_BYTES);
        inSeries++;
      }
      keys[i].indexTenths = record.seriesIndexTenths;
      keys[i].ordinal = i;
    }
    std::sort(keys.get(), keys.get() + head.count, [](const SeriesKey& a, const SeriesKey& b) {
      if (a.inSeries != b.inSeries) return a.inSeries;
      if (a.inSeries) {
        const int c = memcmp(a.key, b.key, CATALOG_FOLD_BYTES);
        if (c != 0) return c < 0;
        if (a.indexTenths != b.indexTenths) return a.indexTenths < b.indexTenths;
      }
      return a.ordinal < b.ordinal;
    });
    for (uint16_t i = 0; i < head.count; i++) ordinals[i] = keys[i].ordinal;
    keys.reset();
    if (!writeIndexFile(catalogSeriesIndexPath(accountKey), head, ordinals.get(), inSeries)) allOk = false;
  }
  return allOk;
}

}  // namespace bookorbit
