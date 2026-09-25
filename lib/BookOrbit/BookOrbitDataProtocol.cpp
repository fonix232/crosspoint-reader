#include "BookOrbitDataProtocol.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "BookOrbitCatalogParser.h"

namespace bookorbit {

void deviceEnvelope(JsonDocument& doc, const std::string& deviceId, const std::string& model) {
  doc["deviceId"] = deviceId;
  doc["deviceModel"] = model;
  doc["pluginVersion"] = "crosspoint-bo-2";
}

void annotationChange(JsonObject out, const AnnotationKind kind, const Annotation& entry) {
  out["datetime"] = entry.datetime;
  if (!entry.updated.empty()) out["datetimeUpdated"] = entry.updated;
  out[kind == AnnotationKind::Bookmark ? "pos" : "pos0"] = entry.pos;
  if (!entry.title.empty()) out["chapter"] = entry.title;
  if (!entry.note.empty()) out["note"] = entry.note;
  if (kind == AnnotationKind::Highlight) {
    out["drawer"] = "lighten";
    out["posFormat"] = "xpointer";
    out["text"] = entry.text;
    if (!entry.end.empty()) out["pos1"] = entry.end;
  }
}

void annotationAck(JsonObject book, const AnnotationKind kind, const Annotation& entry) {
  JsonArray list = book[entry.deleted ? "deleted" : "applied"].as<JsonArray>();
  if (list.isNull()) list = book[entry.deleted ? "deleted" : "applied"].to<JsonArray>();
  JsonObject ack = list.add<JsonObject>();
  ack["serverId"] = entry.serverId;
  ack["status"] = "applied";
  if (entry.deleted) return;
  if (kind == AnnotationKind::Highlight) {
    ack["version"] = entry.version;
    ack["verified"] = entry.mapped;
  } else {
    ack["key"] = entry.key;
    ack["datetime"] = entry.datetime;
    ack["pos"] = entry.pos;
  }
}

bool validDocumentHash(const std::string& value) {
  if (value.size() != 32) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
}

bool annotationAcknowledged(const std::string& response, const std::string& hash, bool deletion) {
  JsonDocument doc;
  if (deserializeJson(doc, response)) return false;
  for (JsonVariantConst unknown : doc["unmatched"].as<JsonArrayConst>()) {
    if (hash == (unknown.as<const char*>() ? unknown.as<const char*>() : "")) return false;
  }
  for (JsonObjectConst result : doc["results"].as<JsonArrayConst>()) {
    if (hash != (result["hash"] | "") || !result["acked"].is<unsigned>()) continue;
    // Bookmark deletion acknowledgements remove the device link. A retry
    // after a lost response therefore confirms zero newly removed links.
    const unsigned count = result["acked"].as<unsigned>();
    return count == 1 || (deletion && count == 0);
  }
  return false;
}

bool statsAccepted(const std::string& response, const std::string& hash, const size_t count) {
  JsonDocument doc;
  if (deserializeJson(doc, response)) return false;
  for (JsonVariantConst unknown : doc["unmatched"].as<JsonArrayConst>()) {
    if (hash == (unknown.as<const char*>() ? unknown.as<const char*>() : "")) return false;
  }
  for (JsonObjectConst result : doc["results"].as<JsonArrayConst>()) {
    if (hash != (result["hash"] | "")) continue;
    const size_t accepted = result["accepted"] | 0u;
    const size_t duplicates = result["duplicates"] | 0u;
    return accepted <= count && duplicates <= count && accepted + duplicates == count;
  }
  return false;
}

std::string encodeQuery(const std::string& value) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += HEX_DIGITS[c >> 4];
      out += HEX_DIGITS[c & 15];
    }
  }
  return out;
}

AnnotationReplyParser::AnnotationReplyParser(char* buffer, size_t capacity, Callback callback, void* context,
                                             std::string expectedHash)
    : parser(JsonCallbacks{this, key, string, number, boolean, nullptr, objectStart, objectEnd, arrayStart, arrayEnd,
                           overflow},
             buffer, capacity),
      callback(callback),
      context(context),
      expectedHash(std::move(expectedHash)) {}

void AnnotationReplyParser::feed(const char* bytes, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    if (ended && bytes[i] != ' ' && bytes[i] != '\r' && bytes[i] != '\n' && bytes[i] != '\t') {
      failed = true;
      return;
    }
    parser.feed(bytes + i, 1);
  }
}

void AnnotationReplyParser::key(void* ctx, const char* value, size_t size) {
  auto& self = *static_cast<AnnotationReplyParser*>(ctx);
  self.field.assign(value, size);
}

void AnnotationReplyParser::string(void* ctx, const char* value, size_t size) {
  auto& self = *static_cast<AnnotationReplyParser*>(ctx);
  if (self.depth == 2 && self.field == "hash") {
    if (!self.expectedHash.empty() && self.expectedHash != std::string(value, size))
      self.failed = true;
    else
      self.matchedResult = true;
  }
  if (self.inUnmatched && size != 0) self.unknownBook = true;
  if (self.changeDepth == 0 || self.depth != self.changeDepth) return;
  std::string* target = nullptr;
  if (self.field == "key") target = &self.current.key;
  if (self.field == "datetime") target = &self.current.datetime;
  if (self.field == "datetimeUpdated") target = &self.current.updated;
  if (self.field == "pos" || self.field == "pos0") target = &self.current.pos;
  if (self.field == "pos1") target = &self.current.end;
  if (self.field == "text") target = &self.current.text;
  if (self.field == "title" || self.field == "chapter") target = &self.current.title;
  if (self.field == "note") target = &self.current.note;
  if (target) {
    target->assign(value, size);
    target->resize(decodeJsonUnicodeEscapes(target->data(), target->size()));
    if (target->size() > 4096) self.invalidItem = true;
  }
}

void AnnotationReplyParser::number(void* ctx, const char* value, size_t) {
  auto& self = *static_cast<AnnotationReplyParser*>(ctx);
  const auto n = static_cast<uint32_t>(strtoul(value, nullptr, 10));
  if (self.field == "skippedNoPosition" && n > 0) self.morePending = true;
  if (self.changeDepth == 0 || self.depth != self.changeDepth) return;
  if (self.field == "serverId") self.current.serverId = n;
  if (self.field == "version") self.current.version = n;
}

void AnnotationReplyParser::boolean(void* ctx, bool value) {
  auto& self = *static_cast<AnnotationReplyParser*>(ctx);
  if (self.field == "more" && value) self.morePending = true;
}

void AnnotationReplyParser::objectStart(void* ctx) {
  auto& self = *static_cast<AnnotationReplyParser*>(ctx);
  ++self.depth;
  if (self.changesArray == self.arrayDepth && self.changeDepth == 0) {
    self.changeDepth = self.depth;
    self.current = Annotation{};
    self.current.deleted = self.deleting;
    self.current.edit = self.editing;
    self.current.dirty = false;
    self.current.pendingAck = true;
    self.invalidItem = !self.expectedHash.empty() && !self.matchedResult;
  }
}

void AnnotationReplyParser::objectEnd(void* ctx) {
  auto& self = *static_cast<AnnotationReplyParser*>(ctx);
  if (self.changeDepth == self.depth) {
    const bool valid =
        self.current.serverId > 0 &&
        ((self.current.deleted || self.current.edit) ? validDocumentHash(self.current.key) : !self.current.pos.empty());
    if (!self.invalidItem && valid) {
      if (self.callback && !self.callback(self.context, self.current))
        self.failed = true;
      else
        ++self.delivered;
    } else {
      self.morePending = true;  // Never acknowledge a truncated or unplaceable record.
    }
    self.changeDepth = 0;
  }
  if (--self.depth == 0) self.ended = true;
}

void AnnotationReplyParser::arrayStart(void* ctx) {
  auto& self = *static_cast<AnnotationReplyParser*>(ctx);
  ++self.arrayDepth;
  if (self.depth == 1 && self.field == "unmatched") self.inUnmatched = true;
  if (self.depth == 3 && (self.field == "add" || self.field == "edit" || self.field == "delete")) {
    self.changesArray = self.arrayDepth;
    self.deleting = self.field == "delete";
    self.editing = self.field == "edit";
  }
}

void AnnotationReplyParser::arrayEnd(void* ctx) {
  auto& self = *static_cast<AnnotationReplyParser*>(ctx);
  if (self.arrayDepth == self.changesArray) self.changesArray = -1;
  if (self.depth == 1) self.inUnmatched = false;
  --self.arrayDepth;
}

void AnnotationReplyParser::overflow(void* ctx) {
  auto& self = *static_cast<AnnotationReplyParser*>(ctx);
  if (self.changeDepth != 0) self.invalidItem = true;
}

}  // namespace bookorbit
