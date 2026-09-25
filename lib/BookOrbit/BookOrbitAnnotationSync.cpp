#include "BookOrbitAnnotationSync.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Memory.h>
#include <ProgressMapper.h>

#include <algorithm>

#include "../../src/util/BookmarkFile.h"
#include "../../src/util/BookmarkUtil.h"
#include "BookOrbitCredentialStore.h"
#include "BookOrbitPaths.h"
#include "BookOrbitSyncState.h"

namespace bookorbit {
namespace {
constexpr size_t TOKEN_SIZE = 8192;
struct Exchange {
  AnnotationStore& store;
  AnnotationKind kind;
  std::vector<BookmarkEntry>& bookmarks;
  bool (*shouldCancel)(void*);
  void* cancelContext;
  bool bookmarksChanged = false;

  bool applyBookmark(const Annotation& entry) {
    auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const BookmarkEntry& b) {
      return b.syncDatetime == entry.datetime && b.xpath == entry.pos;
    });
    if (entry.deleted) {
      if (it != bookmarks.end()) {
        bookmarks.erase(it);
        bookmarksChanged = true;
      }
      return true;
    }
    if (!entry.mapped) return false;
    if (it == bookmarks.end()) {
      if (bookmarks.size() >= 500) return false;
      bookmarks.emplace_back();
      it = bookmarks.end() - 1;
    }
    it->syncDatetime = entry.datetime;
    it->xpath = entry.pos;
    it->name = entry.title.substr(0, BookmarkEntry::MAX_NAME_LENGTH);
    it->summary = entry.text.substr(0, BookmarkEntry::MAX_NAME_LENGTH);
    it->computedSpineIndex = entry.spine;
    it->hasVisibleTextOffset = true;
    it->visibleTextOffset = entry.offset;
    it->percentage = entry.percentage;
    bookmarksChanged = true;
    return true;
  }

  static bool receive(void* ctx, const Annotation& incoming) {
    auto& self = *static_cast<Exchange*>(ctx);
    if (self.shouldCancel && self.shouldCancel(self.cancelContext)) return false;
    Annotation entry = incoming, previous;
    bool known = (!incoming.key.empty() && self.store.find(incoming.key, previous)) ||
                 self.store.findServer(incoming.serverId, previous);
    if (incoming.deleted || incoming.edit) {
      // Never apply a positionless edit/delete to a guessed local identity.
      if (!known) return true;
      entry = previous;
      entry.serverId = incoming.serverId;
      entry.version = incoming.version;
      entry.pendingAck = true;
      entry.dirty = false;
      if (incoming.deleted)
        entry.deleted = true;
      else {
        entry.updated = incoming.updated;
        entry.text = incoming.text;
        entry.note = incoming.note;
        entry.title = incoming.title;
      }
    } else {
      if (known) {
        entry.key = previous.key;
        entry.datetime = previous.datetime;
      } else if (!annotationIdentity(entry, nowIfClockValid()))
        return false;
      // Repeated delivery must not discard a successfully cached translation.
      if (known && previous.mapped && previous.pos == entry.pos && previous.end == entry.end) {
        entry.mapped = true;
        entry.spine = previous.spine;
        entry.offset = previous.offset;
        entry.endOffset = previous.endOffset;
        entry.percentage = previous.percentage;
      }
    }
    // Persist pending acknowledgements before touching the reader's bookmark
    // file. A crash at either step can replay the same identity safely.
    if (!self.store.put(entry)) return false;
    return self.kind != AnnotationKind::Bookmark || (!entry.deleted && !entry.mapped) || self.applyBookmark(entry);
  }
};

bool parseResponse(const std::string& path, char* token, AnnotationReplyParser::Callback callback, void* ctx,
                   bool& more, const std::string& hash) {
  HalFile input;
  if (!Storage.openFileForRead("BOANN", path, input)) return false;
  auto parser = makeUniqueNoThrow<AnnotationReplyParser>(token, TOKEN_SIZE, callback, ctx, hash);
  if (!parser) return false;
  char bytes[192];
  int n;
  while ((n = input.read(bytes, sizeof(bytes))) > 0) parser->feed(bytes, n);
  more = parser->more();
  return n == 0 && parser->complete() && !parser->unmatched();
}

ClientError acknowledge(Client& client, AnnotationStore& store, AnnotationKind kind, const std::string& hash,
                        const std::string& route, bool (*shouldCancel)(void*), void* cancelContext) {
  Annotation entry;
  for (unsigned i = 0; i < store.count(); ++i) {
    if (!store.read(i, entry)) return ClientError::FILE;
    if (!entry.pendingAck) continue;
    if (shouldCancel && shouldCancel(cancelContext)) return ClientError::ABORTED;
    JsonDocument doc;
    deviceEnvelope(doc, BOOKORBIT_STORE.getDeviceId(), "CrossPoint");
    JsonObject book = doc["books"].to<JsonArray>().add<JsonObject>();
    book["hash"] = hash;
    book["applied"].to<JsonArray>();
    book["deleted"].to<JsonArray>();
    annotationAck(book, kind, entry);
    std::string request, response;
    serializeJson(doc, request);
    doc.clear();
    auto error = client.postJson(route + "-ack", request, response);
    if (error != ClientError::OK) return error;
    if (!annotationAcknowledged(response, hash, entry.deleted)) return ClientError::JSON;
    entry.pendingAck = false;
    if (!store.put(entry)) return ClientError::FILE;
  }
  return ClientError::OK;
}
}  // namespace

bool mapPendingAnnotations(const std::string& account, const std::shared_ptr<Epub>& epub, GfxRenderer& renderer) {
  if (!epub) return false;
  // Reuse one record across the scan; annotation strings exceed the C3 stack budget.
  std::unique_ptr<Annotation> entry;
  std::vector<BookmarkEntry> bookmarks;
  bool bookmarksLoaded = false;
  bool allMapped = true;
  const auto& path = epub->getPath();
  for (const auto kind : {AnnotationKind::Bookmark, AnnotationKind::Highlight}) {
    AnnotationStore store(annotationDirectory(account, path, kind));
    if (!store.open()) return false;
    if (!store.count()) continue;
    if (!entry) entry = makeUniqueNoThrow<Annotation>();
    if (!entry) return false;
    Exchange exchange{store, kind, bookmarks, nullptr, nullptr};
    for (unsigned i = 0; i < store.count(); ++i) {
      if (!store.read(i, *entry)) return false;
      if (entry->deleted || entry->mapped) continue;
      if (kind == AnnotationKind::Bookmark && !bookmarksLoaded) {
        bookmarks.reserve(32);
        const auto bookmarkPath = BookmarkUtil::getBookmarkPath(path);
        if ((Storage.exists(bookmarkPath.c_str()) || Storage.exists((bookmarkPath + ".bak").c_str())) &&
            !BookmarkFile::load(path, bookmarks))
          return false;
        bookmarksLoaded = true;
      }
      {
        GfxRenderer::FrameBufferLoan loan(renderer);
        const auto start = ProgressMapper::toCrossPoint(epub, {entry->pos, 0}, renderer);
        if (!start.hasVisibleTextOffset) {
          allMapped = false;
          continue;
        }
        entry->spine = start.spineIndex;
        entry->offset = start.visibleTextOffset;
        entry->endOffset = entry->offset;
        if (!entry->end.empty()) {
          const auto end = ProgressMapper::toCrossPoint(epub, {entry->end, 0}, renderer);
          // Retain the original range for retry if either endpoint cannot be mapped.
          if (!end.hasVisibleTextOffset || end.spineIndex != start.spineIndex) {
            allMapped = false;
            continue;
          }
          entry->endOffset = end.visibleTextOffset;
        }
      }
      entry->mapped = true;
      entry->percentage = epub->calculateProgress(entry->spine, 0);
      // Save the reader bookmark first. If the record write fails, replay updates
      // the same identity rather than inferring that the bookmark was deleted.
      if (kind == AnnotationKind::Bookmark && (!exchange.applyBookmark(*entry) || !BookmarkFile::save(path, bookmarks)))
        return false;
      if (entry->serverId) entry->pendingAck = true;
      if (!store.put(*entry)) return false;
    }
  }
  return allMapped;
}

ClientError syncAnnotations(Client& client, const std::string& account, const std::string& path,
                            const std::string& hash, AnnotationKind kind, bool (*shouldCancel)(void*),
                            void* cancelContext) {
  AnnotationStore store(annotationDirectory(account, path, kind));
  if (!store.open()) return ClientError::FILE;
  std::vector<BookmarkEntry> bookmarks;
  bookmarks.reserve(32);
  bool localBookmarksPresent = false;
  if (kind == AnnotationKind::Bookmark) {
    localBookmarksPresent = Storage.exists(BookmarkUtil::getBookmarkPath(path).c_str()) ||
                            Storage.exists((BookmarkUtil::getBookmarkPath(path) + ".bak").c_str());
    if (localBookmarksPresent && !BookmarkFile::load(path, bookmarks)) return ClientError::FILE;
  }
  const std::string route = kind == AnnotationKind::Bookmark ? "/koreader/plugin/bookmarks/exchange"
                                                             : "/koreader/plugin/annotations/exchange";
  Exchange exchange{store, kind, bookmarks, shouldCancel, cancelContext};
  Annotation entry;
  for (unsigned i = 0; i < store.count(); ++i) {
    if (!store.read(i, entry)) return ClientError::FILE;
    // A missing bookmark file is not data loss when all server entries are still
    // waiting for their first translation.
    if (kind == AnnotationKind::Bookmark && !localBookmarksPresent && store.hasHistory() && entry.mapped &&
        !entry.deleted && !entry.pendingAck)
      return ClientError::FILE;
    if (entry.pendingAck && kind == AnnotationKind::Bookmark && (entry.deleted || entry.mapped) &&
        !exchange.applyBookmark(entry))
      return ClientError::FILE;
  }
  if (exchange.bookmarksChanged && !BookmarkFile::save(path, bookmarks)) return ClientError::FILE;
  auto error = acknowledge(client, store, kind, hash, route, shouldCancel, cancelContext);
  if (error != ClientError::OK) return error;

  if (kind == AnnotationKind::Bookmark) {
    bool identitiesChanged = false;
    for (auto& b : bookmarks) {
      Annotation local;
      local.pos = b.xpath;
      local.datetime = b.syncDatetime;
      if (!annotationIdentity(local, nowIfClockValid())) return ClientError::FILE;
      if (b.syncDatetime.empty()) {
        b.syncDatetime = local.datetime;
        identitiesChanged = true;
      }
      Annotation old;
      const bool known = store.find(local.key, old);
      if (known) local = old;
      const std::string title =
          known && !b.name.empty() && b.name == old.title.substr(0, BookmarkEntry::MAX_NAME_LENGTH)
              ? old.title
              : (b.name.empty() ? b.summary : b.name);
      const bool changed = !known || local.title != title || local.deleted || local.spine != b.computedSpineIndex ||
                           local.offset != b.visibleTextOffset || local.mapped != b.hasVisibleTextOffset;
      if (local.title != title || local.deleted) {
        local.dirty = true;
        Annotation date;
        date.pos = local.pos;
        if (!annotationIdentity(date, nowIfClockValid())) return ClientError::FILE;
        local.updated = date.datetime;
      }
      local.title = title;
      local.deleted = false;
      local.spine = b.computedSpineIndex;
      local.offset = b.visibleTextOffset;
      local.mapped = b.hasVisibleTextOffset;
      local.percentage = b.percentage;
      if (changed && !store.put(local)) return ClientError::FILE;
    }
    if (identitiesChanged && !BookmarkFile::save(path, bookmarks)) return ClientError::FILE;
    for (unsigned i = 0; i < store.count(); ++i) {
      if (!store.read(i, entry)) return ClientError::FILE;
      if (entry.deleted || !entry.mapped) continue;
      const bool present = std::any_of(bookmarks.begin(), bookmarks.end(), [&](const BookmarkEntry& b) {
        return b.xpath == entry.pos && b.syncDatetime == entry.datetime;
      });
      if (!present && localBookmarksPresent && !store.eraseLocal(entry.key)) return ClientError::FILE;
    }
  }

  auto token = makeUniqueNoThrow<char[]>(TOKEN_SIZE);
  if (!token) return ClientError::LOW_MEMORY;
  const std::string responsePath = accountDir(account) + "/annotation-response.tmp";
  for (unsigned batch = 0; batch < 8; ++batch) {
    if (shouldCancel && shouldCancel(cancelContext)) return ClientError::ABORTED;
    JsonDocument doc;
    deviceEnvelope(doc, BOOKORBIT_STORE.getDeviceId(), "CrossPoint");
    JsonObject book = doc["books"].to<JsonArray>().add<JsonObject>();
    book["hash"] = hash;
    auto keys = book["keys"].to<JsonArray>();
    auto changes = book["changes"].to<JsonArray>();
    std::string uploaded;
    unsigned alive = 0;
    for (unsigned i = 0; i < store.count(); ++i) {
      if (!store.read(i, entry)) return ClientError::FILE;
      if (entry.deleted) continue;
      ++alive;
      if (keys.size() < 40) {
        auto key = keys.add<JsonObject>();
        key["k"] = entry.key;
        key["dt"] = entry.datetime;
      }
      if (entry.dirty && uploaded.empty()) {
        annotationChange(changes.add<JsonObject>(), kind, entry);
        uploaded = entry.key;
      }
    }
    book["keysComplete"] = store.hasHistory() && keys.size() == alive;
    if (doc.overflowed() || measureJson(doc) > 12 * 1024) return ClientError::LOW_MEMORY;
    std::string request;
    serializeJson(doc, request);
    doc.clear();
    struct Sink {
      HalFile file;
      size_t bytes = 0;
    } sink;
    if (!Storage.openFileForWrite("BOANN", responsePath, sink.file)) return ClientError::FILE;
    error = client.postStream(
        route, request,
        [](void* ctx, const char* bytes, size_t size) {
          auto& sink = *static_cast<Sink*>(ctx);
          sink.bytes += size;
          return sink.bytes <= 1024 * 1024 && sink.file.write(reinterpret_cast<const uint8_t*>(bytes), size) == size;
        },
        &sink);
    if (!sink.file.close()) return ClientError::FILE;
    if (error != ClientError::OK) return error;
    bool more = false;
    if (!parseResponse(responsePath, token.get(), nullptr, nullptr, more, hash)) return ClientError::JSON;
    exchange.bookmarksChanged = false;
    if (!parseResponse(responsePath, token.get(), Exchange::receive, &exchange, more, hash)) return ClientError::FILE;
    if (exchange.bookmarksChanged && !BookmarkFile::save(path, bookmarks)) return ClientError::FILE;
    if (!uploaded.empty()) {
      if (!store.find(uploaded, entry)) return ClientError::FILE;
      entry.dirty = false;
      if (!store.put(entry)) return ClientError::FILE;
    }
    error = acknowledge(client, store, kind, hash, route, shouldCancel, cancelContext);
    if (error != ClientError::OK) return error;
    if (!more && !store.markHistory()) return ClientError::FILE;
    Storage.remove(responsePath.c_str());
    if (!more && uploaded.empty()) break;
  }
  return ClientError::OK;
}
}  // namespace bookorbit
