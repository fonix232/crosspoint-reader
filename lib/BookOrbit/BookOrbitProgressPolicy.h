#pragma once

#include <cstdint>
#include <string_view>

// Progress arbitration for one managed file. Pure: the sync session gathers
// the three inputs (current local progress, the server's record, and what the
// two sides last agreed on) and acts on the decision.
//
// The rules follow the integration plan:
//   1. Server unchanged since the last acknowledgement -> upload the local
//      position, including intentional backward movement.
//   2. Local unchanged, server newer -> apply the remote location (the book
//      is closed during a session, so this is the safe boundary).
//   3. Both changed -> conflict; the user picks. Never "the larger percentage".
// Identity changes (a different file version) are handled before this point
// because the document hash is derived from the exact bytes.
namespace bookorbit {

enum class SyncDecision : uint8_t { NothingToDo, Upload, ApplyRemote, Conflict };

struct LocalProgressState {
  bool hasProgress = false;
  uint16_t spine = 0;
  uint32_t key = 0;  // localProgressKey()
};

struct RemoteProgressState {
  bool found = false;
  bool fromThisDevice = false;  // the record is our own earlier upload
  uint32_t timestamp = 0;
  uint32_t hash = 0;  // locatorHash()
};

struct AckState {
  bool hasLocalAck = false;
  uint16_t ackSpine = 0;
  uint32_t ackKey = 0;
  bool hasRemoteAck = false;
  uint32_t ackTimestamp = 0;
  uint32_t ackHash = 0;
};

// Content key for a saved local position: the visible-text offset when the
// reader recorded one, else the page number tagged so the two never collide.
inline uint32_t localProgressKey(const bool hasOffset, const uint32_t offset, const uint16_t page) {
  return hasOffset ? (offset & 0x7FFFFFFFu) : (0x80000000u | page);
}

// FNV-1a over the locator text and the quantized percentage, so a record that
// moved only by percentage (paged formats) still reads as changed.
inline uint32_t locatorHash(const std::string_view locator, const float percentage) {
  uint32_t hash = 2166136261u;
  for (const unsigned char c : locator) {
    hash ^= c;
    hash *= 16777619u;
  }
  const uint32_t pctQ = static_cast<uint32_t>((percentage < 0 ? 0 : percentage > 1 ? 1 : percentage) * 1000000.0f);
  for (int i = 0; i < 4; i++) {
    hash ^= (pctQ >> (i * 8)) & 0xFF;
    hash *= 16777619u;
  }
  return hash;
}

inline bool localChangedSinceAck(const LocalProgressState& local, const AckState& ack) {
  if (!local.hasProgress) return false;
  return !ack.hasLocalAck || ack.ackSpine != local.spine || ack.ackKey != local.key;
}

inline bool remoteChangedSinceAck(const RemoteProgressState& remote, const AckState& ack) {
  if (!remote.found) return false;
  if (!ack.hasRemoteAck) return !remote.fromThisDevice;
  return ack.ackTimestamp != remote.timestamp || ack.ackHash != remote.hash;
}

inline SyncDecision decideSync(const LocalProgressState& local, const RemoteProgressState& remote,
                               const AckState& ack) {
  const bool localChanged = localChangedSinceAck(local, ack);
  // Our own upload coming back is an acknowledgement, not a competing change.
  const bool remoteChanged = remoteChangedSinceAck(remote, ack) && !remote.fromThisDevice;
  if (localChanged && remoteChanged) return SyncDecision::Conflict;
  if (localChanged) return SyncDecision::Upload;
  if (remoteChanged) return SyncDecision::ApplyRemote;
  return SyncDecision::NothingToDo;
}

}  // namespace bookorbit
