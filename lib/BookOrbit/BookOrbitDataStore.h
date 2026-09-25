#pragma once

#include <string>

#include "BookOrbitDataProtocol.h"

namespace bookorbit {

std::string localDataKey(const std::string& bookPath);
std::string annotationDirectory(const std::string& account, const std::string& bookPath, AnnotationKind kind);
bool annotationIdentity(Annotation& entry, uint32_t epoch);

// The index contains only fixed 32-byte keys. Text and locators live in separate
// atomic JSON records, so a heavily annotated book never has to fit in RAM.
class AnnotationStore {
 public:
  explicit AnnotationStore(std::string directory) : directory(std::move(directory)) {}
  bool open();
  unsigned count() const { return total; }
  bool read(unsigned ordinal, Annotation& out) const;
  bool find(const std::string& key, Annotation& out) const;
  bool findServer(uint32_t id, Annotation& out) const;
  bool put(const Annotation& entry);
  bool hasHistory() const { return history; }
  bool markHistory();
  bool eraseLocal(const std::string& key);

 private:
  std::string pathFor(const std::string& key) const;
  std::string directory;
  unsigned total = 0;
  bool history = false;
};

// One fixed record per completed page. Successful uploads archive their resolved
// events before removing the acknowledged prefix, preserving concurrent appends.
struct ReadingEvent {
  uint32_t boot = 0;
  uint32_t uptime = 0;
  PageStat stat;
};

class ReadingQueue {
 public:
  static constexpr unsigned BATCH = 25;
  static constexpr unsigned MAX_EVENTS = 8192;
  static constexpr uint32_t RETENTION_SECONDS = 30u * 24u * 60u * 60u;
  explicit ReadingQueue(std::string bookPath);
  bool append(const ReadingEvent* events, unsigned count);
  bool read(ReadingEvent* events, unsigned capacity, unsigned& count, unsigned* queued = nullptr) const;
  bool acknowledge(unsigned count);
  bool acknowledgeSynced(const ReadingEvent* events, unsigned count, uint32_t now);
  static bool pruneSynced(uint32_t now);
  bool preserveUndated(const ReadingEvent& event);

 private:
  bool acknowledgeLocked(unsigned count);
  std::string path;
};

}  // namespace bookorbit
