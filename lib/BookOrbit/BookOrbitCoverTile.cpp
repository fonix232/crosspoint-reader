#include "BookOrbitCoverTile.h"

#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace bookorbit {

namespace {
int32_t get32s(const uint8_t* in) {
  return static_cast<int32_t>(static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
                              (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24));
}
uint16_t get16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | (in[1] << 8)); }
}  // namespace

CoverTileSink::CoverTileSink(uint8_t* tile) : tile(tile) { memset(tile, 0, COVER_TILE_BYTES); }

void CoverTileSink::parseHeader() {
  if (header[0] != 'B' || header[1] != 'M' || get16(header + 28) != 1) {
    error = true;
    return;
  }
  dataOffset = static_cast<size_t>(get32s(header + 10));
  width = get32s(header + 18);
  const int32_t rawHeight = get32s(header + 22);
  topDown = rawHeight < 0;
  height = topDown ? -rawHeight : rawHeight;
  if (width <= 0 || height <= 0 || dataOffset < HEADER_BYTES) {
    error = true;
    return;
  }
  rowBytes = static_cast<size_t>((width + 31) / 32) * 4;
  if (rowBytes > MAX_ROW_BYTES) {
    error = true;
    return;
  }
  // Center crop: the converter scaled to cover COVER_W x COVER_H, so one
  // dimension matches and the other is at least as large.
  cropX = width > COVER_W ? (width - COVER_W) / 2 : 0;
  cropY = height > COVER_H ? (height - COVER_H) / 2 : 0;
  headerDone = true;
}

void CoverTileSink::commitRow() {
  const int srcY = topDown ? rowsDone : height - 1 - rowsDone;
  rowsDone++;
  const int tileY = srcY - cropY;
  if (tileY < 0 || tileY >= COVER_H) return;
  const int tileX0 = (COVER_TILE_SIZE - COVER_W) / 2;
  uint8_t* out = tile + static_cast<size_t>(tileY) * COVER_TILE_STRIDE;
  for (int x = 0; x < COVER_W; x++) {
    const int srcX = cropX + x;
    if (srcX >= width) break;
    const uint8_t bit = (row[srcX / 8] >> (7 - (srcX % 8))) & 1u;
    // Palette index 0 is black in the converter's BMP; BW1 set bit = ink.
    if (bit == 0) {
      const int tx = tileX0 + x;
      out[tx / 8] |= static_cast<uint8_t>(0x80u >> (tx % 8));
    }
  }
}

size_t CoverTileSink::write(const uint8_t b) { return write(&b, 1); }

size_t CoverTileSink::write(const uint8_t* buf, const size_t size) {
  if (error) return size;
  size_t i = 0;
  while (i < size) {
    if (!headerDone) {
      header[headerLen++] = buf[i++];
      if (headerLen == HEADER_BYTES) {
        parseHeader();
        if (error) return size;
        skipped = HEADER_BYTES;
      }
      continue;
    }
    if (skipped < dataOffset) {
      skipped++;
      i++;
      continue;
    }
    if (rowsDone >= height) return size;  // trailing bytes are ignored
    const size_t take = std::min(size - i, rowBytes - rowLen);
    memcpy(row + rowLen, buf + i, take);
    rowLen += take;
    i += take;
    if (rowLen == rowBytes) {
      commitRow();
      rowLen = 0;
    }
  }
  return size;
}

bool renderCoverTile(const std::string& jpegPath, uint8_t* tile) {
  HalFile jpeg;
  if (!Storage.openFileForRead("BOCOV", jpegPath.c_str(), jpeg)) return false;
  CoverTileSink sink(tile);
  if (!JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(jpeg, sink, COVER_W, COVER_H)) return false;
  if (!sink.complete()) {
    LOG_ERR("BOCOV", "cover tile incomplete: %s", jpegPath.c_str());
    return false;
  }
  return true;
}

}  // namespace bookorbit
