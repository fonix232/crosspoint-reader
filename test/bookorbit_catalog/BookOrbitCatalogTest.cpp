#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "BookOrbitCatalogFormat.h"
#include "BookOrbitCatalogParser.h"

using namespace bookorbit;

namespace {

struct Collector {
  std::vector<CatalogRecord> records;
  bool acceptAll = true;
  static bool onItem(void* ctx, const CatalogRecord& record) {
    auto* self = static_cast<Collector*>(ctx);
    self->records.push_back(record);
    return self->acceptAll;
  }
};

const char* PAGE_JSON = R"({
  "items": [
    {"id": 12, "title": "The Hobbit", "authors": ["J. R. R. Tolkien"], "seriesId": 4,
     "seriesName": "Middle-earth", "seriesIndex": 1.5, "progressPercentage": 42.4, "lastReadAt": "2025-03-04T05:06:07.000Z",
     "readStatus": "reading", "formats": ["epub"], "hasCover": true,
     "thumbnailUrl": "/api/v1/koreader/plugin/catalog/books/12/thumbnail", "detailUrl": "x",
     "addedAt": "2024-12-31T23:59:59Z", "updatedAt": "2025-01-01T00:00:00Z"},
    {"id": 7, "title": "Café 😀", "authors": ["Ann Lee", "Bob Roe"], "progressPercentage": null,
     "lastReadAt": null, "formats": ["epub", "pdf"], "hasCover": false, "thumbnailUrl": null,
     "detailUrl": "y", "addedAt": "2025-02-01T00:00:00.000Z", "updatedAt": "2025-02-01T00:00:00.000Z",
     "extra": {"nested": [1, 2, {"deep": "ignored"}]}}
  ],
  "total": 1234,
  "page": 1,
  "size": 2,
  "hasNext": true,
  "hasPrevious": false,
  "nextUrl": "/next",
  "previousUrl": null
})";

void feedInChunks(CatalogPageParser& parser, const char* text, const size_t chunk) {
  const size_t len = strlen(text);
  for (size_t pos = 0; pos < len; pos += chunk) {
    parser.feed(text + pos, std::min(chunk, len - pos));
  }
}

}  // namespace

TEST(BookOrbitCatalogParser, ParsesItemsAndPageMetadata) {
  Collector collector;
  CatalogPageParser parser(&Collector::onItem, &collector);
  parser.feed(PAGE_JSON, strlen(PAGE_JSON));

  ASSERT_FALSE(parser.hasError());
  EXPECT_EQ(parser.total(), 1234u);
  EXPECT_TRUE(parser.hasNext());
  EXPECT_EQ(parser.itemCount(), 2u);
  ASSERT_EQ(collector.records.size(), 2u);

  const CatalogRecord& hobbit = collector.records[0];
  EXPECT_EQ(hobbit.bookId, 12u);
  EXPECT_EQ(std::string(hobbit.titleView()), "The Hobbit");
  EXPECT_EQ(std::string(hobbit.authorView()), "J. R. R. Tolkien");
  EXPECT_TRUE(hobbit.hasCover());
  EXPECT_EQ(hobbit.progressPct, 42);
  EXPECT_EQ(hobbit.lastReadAt, 1741064767u);  // 2025-03-04T05:06:07Z
  EXPECT_EQ(hobbit.addedAt, 1735689599u);     // 2024-12-31T23:59:59Z
  // Folded, article-stripped title key and surname-first author key.
  EXPECT_EQ(std::string(hobbit.foldView()), "the hobbit");
  EXPECT_EQ(std::string(hobbit.seriesView()), "Middle-earth");
  EXPECT_EQ(hobbit.seriesIndexTenths, 15);
  EXPECT_EQ(std::string(hobbit.authorFoldView()).rfind("tolkien", 0), 0u);

  const CatalogRecord& cafe = collector.records[1];
  EXPECT_EQ(cafe.bookId, 7u);
  EXPECT_EQ(std::string(cafe.titleView()), "Café 😀");
  EXPECT_EQ(std::string(cafe.authorView()), "Ann Lee, Bob Roe");
  EXPECT_FALSE(cafe.hasCover());
  EXPECT_EQ(cafe.progressPct, CATALOG_PROGRESS_NONE);
  EXPECT_EQ(cafe.lastReadAt, 0u);
  EXPECT_EQ(cafe.seriesLen, 0);
  EXPECT_EQ(cafe.seriesIndexTenths, 0);
}

TEST(BookOrbitCatalogParser, ChunkedFeedMatchesWholeFeed) {
  Collector whole;
  CatalogPageParser wholeParser(&Collector::onItem, &whole);
  wholeParser.feed(PAGE_JSON, strlen(PAGE_JSON));

  for (const size_t chunk : {1u, 3u, 17u, 64u}) {
    Collector chunked;
    CatalogPageParser parser(&Collector::onItem, &chunked);
    feedInChunks(parser, PAGE_JSON, chunk);
    ASSERT_FALSE(parser.hasError()) << "chunk " << chunk;
    ASSERT_EQ(chunked.records.size(), whole.records.size()) << "chunk " << chunk;
    for (size_t i = 0; i < whole.records.size(); i++) {
      uint8_t a[CATALOG_RECORD_BYTES];
      uint8_t b[CATALOG_RECORD_BYTES];
      encodeRecord(whole.records[i], a);
      encodeRecord(chunked.records[i], b);
      EXPECT_EQ(memcmp(a, b, sizeof(a)), 0) << "chunk " << chunk << " record " << i;
    }
  }
}

TEST(BookOrbitCatalogParser, CallbackCanStopTheStream) {
  Collector collector;
  collector.acceptAll = false;
  CatalogPageParser parser(&Collector::onItem, &collector);
  parser.feed(PAGE_JSON, strlen(PAGE_JSON));
  EXPECT_TRUE(parser.aborted());
  EXPECT_EQ(collector.records.size(), 1u);
}

TEST(BookOrbitCatalogParser, MissingIdIsAnError) {
  const char* json = R"({"items":[{"title":"No id"}],"total":1,"hasNext":false})";
  Collector collector;
  CatalogPageParser parser(&Collector::onItem, &collector);
  parser.feed(json, strlen(json));
  EXPECT_TRUE(parser.hasError());
  EXPECT_TRUE(collector.records.empty());
}

TEST(BookOrbitCatalogParser, EmptyTitleFallsBackToId) {
  const char* json = R"({"items":[{"id":99,"title":"","authors":[]}],"total":1,"hasNext":false})";
  Collector collector;
  CatalogPageParser parser(&Collector::onItem, &collector);
  parser.feed(json, strlen(json));
  ASSERT_EQ(collector.records.size(), 1u);
  EXPECT_EQ(std::string(collector.records[0].titleView()), "#99");
}

TEST(BookOrbitCatalogParser, LongTitleTruncatesOnCodepointBoundary) {
  std::string title;
  while (title.size() < CATALOG_TITLE_BYTES - 1) title += "é";  // 2 bytes each
  title += "é";                                                 // straddles the cap
  const std::string json = "{\"items\":[{\"id\":1,\"title\":\"" + title + "\"}],\"total\":1,\"hasNext\":false}";
  Collector collector;
  CatalogPageParser parser(&Collector::onItem, &collector);
  parser.feed(json.c_str(), json.size());
  ASSERT_EQ(collector.records.size(), 1u);
  const auto view = collector.records[0].titleView();
  EXPECT_LE(view.size(), CATALOG_TITLE_BYTES);
  EXPECT_EQ(view.size() % 2, 0u);  // whole "é" sequences only
}

TEST(BookOrbitCatalogFormat, RecordRoundTrips) {
  CatalogRecord in;
  in.bookId = 0xDEADBEEF;
  in.addedAt = 1700000000;
  in.lastReadAt = 1700000001;
  in.flags = CATALOG_FLAG_HAS_COVER;
  in.progressPct = 77;
  in.titleLen = copyUtf8Bounded("Title", in.title, CATALOG_TITLE_BYTES);
  in.authorLen = copyUtf8Bounded("Author", in.author, CATALOG_AUTHOR_BYTES);
  copyUtf8Bounded("title", in.fold, CATALOG_FOLD_BYTES);
  copyUtf8Bounded("author", in.authorFold, CATALOG_FOLD_BYTES);
  in.seriesLen = copyUtf8Bounded("Series", in.series, CATALOG_SERIES_BYTES);
  in.seriesIndexTenths = 30;

  uint8_t raw[CATALOG_RECORD_BYTES];
  encodeRecord(in, raw);
  CatalogRecord out;
  decodeRecord(raw, out);
  EXPECT_EQ(out.bookId, in.bookId);
  EXPECT_EQ(out.addedAt, in.addedAt);
  EXPECT_EQ(out.lastReadAt, in.lastReadAt);
  EXPECT_EQ(out.flags, in.flags);
  EXPECT_EQ(out.progressPct, in.progressPct);
  EXPECT_EQ(std::string(out.titleView()), "Title");
  EXPECT_EQ(std::string(out.authorView()), "Author");
  EXPECT_EQ(std::string(out.foldView()), "title");
  EXPECT_EQ(std::string(out.authorFoldView()), "author");
  EXPECT_EQ(std::string(out.seriesView()), "Series");
  EXPECT_EQ(out.seriesIndexTenths, 30);
}

TEST(BookOrbitCatalogFormat, HeaderRoundTripsAndRejectsGarbage) {
  CatalogHeader in;
  in.count = 999;
  in.serverTotal = 50000;
  in.fetchedAt = 1750000000;
  in.truncated = true;
  uint8_t raw[CATALOG_HEADER_BYTES];
  encodeHeader(in, raw);
  CatalogHeader out;
  ASSERT_TRUE(decodeHeader(raw, out));
  EXPECT_EQ(out.count, 999);
  EXPECT_EQ(out.serverTotal, 50000u);
  EXPECT_EQ(out.fetchedAt, 1750000000u);
  EXPECT_TRUE(out.truncated);

  raw[0] ^= 0xFF;
  EXPECT_FALSE(decodeHeader(raw, out));
  encodeHeader(in, raw);
  raw[4] = CATALOG_VERSION + 1;
  EXPECT_FALSE(decodeHeader(raw, out));
  in.count = CATALOG_MAX_BOOKS + 1;
  encodeHeader(in, raw);
  EXPECT_FALSE(decodeHeader(raw, out));
}

TEST(BookOrbitCatalogFormat, ParsesIso8601Utc) {
  EXPECT_EQ(parseIso8601Utc("1970-01-01T00:00:01Z"), 1u);
  EXPECT_EQ(parseIso8601Utc("2000-03-01T00:00:00.000Z"), 951868800u);
  EXPECT_EQ(parseIso8601Utc("2024-02-29T12:00:00+00:00"), 1709208000u);
  EXPECT_EQ(parseIso8601Utc("2025-06-15"), 1749945600u);
  EXPECT_EQ(parseIso8601Utc("2025-06-15T10:00:00+02:00"), 0u);  // offsets are not accepted
  EXPECT_EQ(parseIso8601Utc("not a date"), 0u);
  EXPECT_EQ(parseIso8601Utc(""), 0u);
  EXPECT_EQ(parseIso8601Utc("2025-13-01T00:00:00Z"), 0u);
}

TEST(BookOrbitCatalogFormat, DecodesJsonUnicodeEscapes) {
  char text[] = "a\\u00e9b\\ud83d\\ude00c\\u";
  const size_t len = decodeJsonUnicodeEscapes(text, strlen(text));
  EXPECT_EQ(std::string(text, len), "aéb😀c\\u");
}

TEST(BookOrbitCatalogFormat, HeaderVersionIsReadableForOlderFiles) {
  CatalogHeader in;
  in.count = 3;
  uint8_t raw[CATALOG_HEADER_BYTES];
  encodeHeader(in, raw);
  EXPECT_EQ(headerVersion(raw), CATALOG_VERSION);
  raw[4] = CATALOG_VERSION_V1;
  EXPECT_EQ(headerVersion(raw), CATALOG_VERSION_V1);
  CatalogHeader out;
  EXPECT_FALSE(decodeHeader(raw, out));  // the cache upgrades it instead
  raw[0] ^= 0xFF;
  EXPECT_EQ(headerVersion(raw), 0);
}

TEST(BookOrbitCatalogFormat, V1RecordIsAPrefixOfV2) {
  // The upgrader copies a v1 record verbatim and zero-fills the series tail.
  CatalogRecord in;
  in.bookId = 42;
  in.titleLen = copyUtf8Bounded("Title", in.title, CATALOG_TITLE_BYTES);
  in.seriesLen = copyUtf8Bounded("Series", in.series, CATALOG_SERIES_BYTES);
  in.seriesIndexTenths = 25;
  uint8_t raw[CATALOG_RECORD_BYTES];
  encodeRecord(in, raw);
  uint8_t upgraded[CATALOG_RECORD_BYTES] = {};
  memcpy(upgraded, raw, CATALOG_RECORD_BYTES_V1);
  CatalogRecord out;
  decodeRecord(upgraded, out);
  EXPECT_EQ(out.bookId, 42u);
  EXPECT_EQ(std::string(out.titleView()), "Title");
  EXPECT_EQ(out.seriesLen, 0);
  EXPECT_EQ(out.seriesIndexTenths, 0);
}

TEST(BookOrbitCatalogParser, CurrentStringSeriesPositionsMatchLegacyNumbers) {
  const char* page = R"({"items":[
    {"id":6,"title":"Light Bringer","seriesName":"Red Rising","seriesIndex":"6"},
    {"id":1,"title":"Red Rising","seriesName":"Red Rising","seriesIndex":"1"},
    {"id":3,"title":"Morning Star","seriesName":"Red Rising","seriesIndex":3},
    {"id":2,"title":"Golden Son","seriesName":"Red Rising","seriesIndex":"2"},
    {"id":5,"title":"Dark Age","seriesName":"Red Rising","seriesIndex":"5"},
    {"id":4,"title":"Iron Gold","seriesName":"Red Rising","seriesIndex":"4"},
    {"id":7,"seriesIndex":"1.5"}, {"id":8,"seriesIndex":1.5},
    {"id":9,"seriesIndex":null}, {"id":10,"seriesIndex":"garbage"},
    {"id":11,"seriesIndex":"NaN"}, {"id":12,"seriesIndex":"2junk"},
    {"id":13,"seriesIndex":"99999999999999999999"}
  ]})";
  for (const size_t chunk : {1u, 7u, 4096u}) {
    Collector collector;
    CatalogPageParser parser(&Collector::onItem, &collector);
    feedInChunks(parser, page, chunk);
    ASSERT_FALSE(parser.hasError());
    ASSERT_EQ(collector.records.size(), 13u);
    for (size_t i = 0; i < 6; ++i) EXPECT_EQ(collector.records[i].seriesIndexTenths, collector.records[i].bookId * 10);
    EXPECT_EQ(collector.records[6].seriesIndexTenths, 15);
    EXPECT_EQ(collector.records[7].seriesIndexTenths, 15);
    for (size_t i = 8; i < 12; ++i) EXPECT_EQ(collector.records[i].seriesIndexTenths, 0);
    EXPECT_EQ(collector.records[12].seriesIndexTenths, 65535);
  }
}

TEST(BookOrbitCatalogParser, TruncatedOrMissingItemsCannotReplaceLiveCatalog) {
  for (const char* body : {"", "{}", "{\"items\":[{\"id\":1}]", "{\"items\":[{\"id\":1}"}) {
    CatalogPageParser parser(nullptr, nullptr);
    parser.feed(body, strlen(body));
    EXPECT_FALSE(parser.complete());
  }
  CatalogPageParser parser(nullptr, nullptr);
  const char* body = "{\"items\":[],\"total\":0,\"hasNext\":false}";
  parser.feed(body, strlen(body));
  EXPECT_TRUE(parser.complete());
}
