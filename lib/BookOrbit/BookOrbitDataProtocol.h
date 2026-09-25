#pragma once

#include <ArduinoJson.h>
#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace bookorbit {

enum class AnnotationKind : uint8_t { Highlight, Bookmark };

struct Annotation {
  std::string key;
  std::string datetime;
  std::string updated;
  std::string pos;
  std::string end;
  std::string text;
  std::string title;
  std::string note;
  uint32_t serverId = 0;
  uint32_t version = 0;
  uint32_t spine = 0;
  uint32_t offset = 0;
  uint32_t endOffset = 0;
  float percentage = 0;
  bool edit = false;
  bool mapped = false;
  bool deleted = false;
  bool pendingAck = false;
  bool dirty = true;
};

struct PageStat {
  uint32_t startTime = 0;
  uint32_t durationSeconds = 0;
  uint32_t page = 0;
  uint32_t totalPages = 10000;
};

void deviceEnvelope(JsonDocument& doc, const std::string& deviceId, const std::string& model);
void annotationChange(JsonObject out, AnnotationKind kind, const Annotation& entry);
void annotationAck(JsonObject book, AnnotationKind kind, const Annotation& entry);
bool annotationAcknowledged(const std::string& response, const std::string& hash, bool deletion);
bool statsAccepted(const std::string& response, const std::string& hash, size_t count);
std::string encodeQuery(const std::string& value);
bool validDocumentHash(const std::string& value);

// One incoming record at a time, with a caller-owned token buffer. Callbacks
// stage records; they must not acknowledge anything until complete() is true.
class AnnotationReplyParser {
 public:
  using Callback = bool (*)(void*, const Annotation&);
  AnnotationReplyParser(char* tokenBuffer, size_t tokenCapacity, Callback callback, void* context,
                        std::string expectedHash = {});
  void feed(const char* bytes, size_t size);
  bool complete() const {
    return ended && depth == 0 && arrayDepth == 0 && !parser.hasError() && !failed &&
           (expectedHash.empty() || matchedResult);
  }
  bool unmatched() const { return unknownBook; }
  bool more() const { return morePending; }
  unsigned count() const { return delivered; }

 private:
  static void key(void*, const char*, size_t);
  static void string(void*, const char*, size_t);
  static void number(void*, const char*, size_t);
  static void boolean(void*, bool);
  static void objectStart(void*);
  static void objectEnd(void*);
  static void arrayStart(void*);
  static void arrayEnd(void*);
  static void overflow(void*);
  JsonSaxParser parser;
  Callback callback;
  void* context;
  std::string expectedHash;
  bool matchedResult = false;
  Annotation current;
  std::string field;
  int depth = 0;
  int changeDepth = 0;
  int arrayDepth = 0;
  int changesArray = -1;
  bool deleting = false;
  bool editing = false;
  bool inUnmatched = false;
  bool failed = false;
  bool invalidItem = false;
  bool ended = false;
  bool unknownBook = false;
  bool morePending = false;
  unsigned delivered = 0;
};

}  // namespace bookorbit
