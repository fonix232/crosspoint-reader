#pragma once

#include <BookOrbitProgressPolicy.h>

#include <cstdint>

// Display-only state. It is refreshed from the saved position and server
// acknowledgements, never used to decide which reading position wins.
enum class ReadingCardSync : uint8_t { Local, Disabled, Pending, Synced, Conflict, Failed, Syncing, Unknown };

inline ReadingCardSync readingCardSync(bool configured, bool enabled, bool managed, bool conflict, bool busy,
                                       bool failed, bool readable, const bookorbit::LocalProgressState& local,
                                       const bookorbit::AckState& ack) {
  if (!configured) return ReadingCardSync::Local;
  if (!enabled) return ReadingCardSync::Disabled;
  if (conflict) return ReadingCardSync::Conflict;
  if (busy) return ReadingCardSync::Syncing;
  if (!readable) return ReadingCardSync::Unknown;
  if (!managed) return ReadingCardSync::Local;
  if (!ack.hasLocalAck || bookorbit::localChangedSinceAck(local, ack))
    return failed ? ReadingCardSync::Failed : ReadingCardSync::Pending;
  // A failed account-wide job does not invalidate this book's acknowledgement.
  // In particular a single-book retry must not need to mark the entire account
  // as synced just to clear its card's error.

  return local.hasProgress ? ReadingCardSync::Synced : ReadingCardSync::Unknown;
}

struct ReadingCardDetails {
  uint32_t bookId = 0;  // exact managed book to check on long press
  char chapter[96] = {};
  int8_t percent = -1;  // unknown, rather than a fabricated zero
  ReadingCardSync sync = ReadingCardSync::Local;
  bool canSync = false;
};
