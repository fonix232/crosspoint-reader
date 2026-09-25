#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "BookOrbitDataProtocol.h"

using namespace bookorbit;
namespace {
bool collect(void* ctx, const Annotation& entry) {
  static_cast<std::vector<Annotation>*>(ctx)->push_back(entry);
  return true;
}
}  // namespace

TEST(BookOrbitData, StreamsAddsEditsAndDeletesWithoutConfusingKeysWithPositions) {
  const std::string json =
      R"({"results":[{"hash":"x","toApply":{"add":[{"serverId":1,"version":2,"datetime":"2026-09-20 10:00:00","pos0":"/body/p[1]","text":"caf\u00e9"}],"edit":[{"serverId":2,"version":3,"key":"11111111111111111111111111111111","text":"updated"}],"delete":[{"serverId":3,"key":"01234567890123456789012345678901"}]},"more":true}],"unmatched":[]})";
  std::array<char, 8192> buffer{};
  std::vector<Annotation> entries;
  AnnotationReplyParser parser(buffer.data(), buffer.size(), collect, &entries);
  for (size_t i = 0; i < json.size(); ++i) parser.feed(json.data() + i, 1);
  ASSERT_TRUE(parser.complete());
  ASSERT_FALSE(parser.unmatched());
  ASSERT_TRUE(parser.more());
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].text, "café");
  EXPECT_EQ(entries[1].version, 3u);
  EXPECT_TRUE(entries[2].deleted);
  EXPECT_TRUE(entries[2].pos.empty());
}

TEST(BookOrbitData, OversizedAndTruncatedChangesAreNotAcknowledged) {
  std::array<char, 128> buffer{};
  std::vector<Annotation> entries;
  AnnotationReplyParser parser(buffer.data(), buffer.size(), collect, &entries);
  const std::string body = "{\"results\":[{\"toApply\":{\"add\":[{\"serverId\":1,\"pos0\":\"/p\",\"text\":\"" +
                           std::string(500, 'x') + "\"}]}}]}";
  parser.feed(body.data(), body.size());
  EXPECT_TRUE(parser.complete());
  EXPECT_TRUE(parser.more());
  EXPECT_TRUE(entries.empty());
  AnnotationReplyParser truncated(buffer.data(), buffer.size(), collect, &entries);
  truncated.feed(body.data(), body.size() - 2);
  EXPECT_FALSE(truncated.complete());
}

TEST(BookOrbitData, StatsNeedFullConfirmationIncludingDuplicateRetries) {
  const std::string hash(32, 'a');
  EXPECT_TRUE(statsAccepted("{\"results\":[{\"hash\":\"" + hash + "\",\"accepted\":0,\"duplicates\":25}]}", hash, 25));
  EXPECT_FALSE(statsAccepted("{\"results\":[{\"hash\":\"" + hash + "\",\"accepted\":24}]}", hash, 25));
  EXPECT_FALSE(statsAccepted("{}", hash, 25));
}

TEST(BookOrbitData, DeletionAcksOmitVersionAndBookmarkAddsCarryIdentity) {
  JsonDocument doc;
  auto book = doc.to<JsonObject>();
  Annotation entry;
  entry.serverId = 5;
  entry.version = 7;
  entry.deleted = true;
  annotationAck(book, AnnotationKind::Highlight, entry);
  EXPECT_TRUE(book["deleted"][0]["version"].isNull());
  entry.deleted = false;
  entry.key = std::string(32, 'a');
  entry.datetime = "2026-09-20 10:00:00";
  entry.pos = "/p";
  annotationAck(book, AnnotationKind::Bookmark, entry);
  EXPECT_EQ(book["applied"][0]["key"].as<std::string>(), entry.key);
}

TEST(BookOrbitData, HighlightReceiptDistinguishesStoredFromMapped) {
  JsonDocument doc;
  auto book = doc.to<JsonObject>();
  Annotation entry;
  entry.serverId = 5;
  entry.version = 7;
  annotationAck(book, AnnotationKind::Highlight, entry);
  EXPECT_FALSE(book["applied"][0]["verified"].as<bool>());
  entry.mapped = true;
  annotationAck(book, AnnotationKind::Highlight, entry);
  EXPECT_TRUE(book["applied"][1]["verified"].as<bool>());
}

TEST(BookOrbitData, QueryEncodingPreservesUnicodeAndCannotInjectFilters) {
  EXPECT_EQ(encodeQuery("a&libraryId=2 café"), "a%26libraryId%3D2%20caf%C3%A9");
}

TEST(BookOrbitData, RefusesWrongBookEmptyEnvelopeAndTrailingGarbage) {
  std::array<char, 8192> buffer{};
  const std::string hash(32, 'a');
  for (const auto& body : {std::string("{}"), std::string("{\"results\":[{\"hash\":\"wrong\"}]}"),
                           std::string("{\"results\":[{\"hash\":\"") + hash + "\"}]}garbage"}) {
    AnnotationReplyParser parser(buffer.data(), buffer.size(), nullptr, nullptr, hash);
    parser.feed(body.data(), body.size());
    EXPECT_FALSE(parser.complete());
  }
}

TEST(BookOrbitData, LostBookmarkDeletionAckMayConfirmZeroNewlyRemovedLinks) {
  const std::string hash(32, 'a');
  const std::string response = "{\"results\":[{\"hash\":\"" + hash + "\",\"acked\":0}]}";
  EXPECT_TRUE(annotationAcknowledged(response, hash, true));
  EXPECT_FALSE(annotationAcknowledged(response, hash, false));
  EXPECT_FALSE(annotationAcknowledged("{}", hash, true));
  EXPECT_FALSE(annotationAcknowledged("{\"results\":[{\"hash\":\"" + hash + "\"}]}", hash, true));
}
