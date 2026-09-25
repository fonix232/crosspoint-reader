#pragma once

#include <Print.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace bookorbit {

// Library row thumbnails: a square BW1 tile (FreeInkUI's set-bit-is-ink,
// MSB-first, row-major format) holding a 2:3 cover centered horizontally.
// The square shape lets the list treat it like any other row icon; the
// blank side columns are transparent.
inline constexpr int COVER_TILE_SIZE = 48;
inline constexpr int COVER_W = 32;
inline constexpr int COVER_H = 48;
inline constexpr size_t COVER_TILE_STRIDE = (COVER_TILE_SIZE + 7) / 8;
inline constexpr size_t COVER_TILE_BYTES = COVER_TILE_STRIDE * COVER_TILE_SIZE;

// Consumes a 1-bit BMP as JpegToBmpConverter streams it (62-byte header,
// palette 0 = black, 4-byte padded rows, top-down or bottom-up) and center-crops
// it into a tile. Nothing beyond one source row is buffered.
class CoverTileSink : public Print {
 public:
  explicit CoverTileSink(uint8_t* tile);

  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buf, size_t size) override;

  // True once every row that intersects the crop window has been written.
  bool complete() const { return !error && headerDone && rowsDone >= height; }

 private:
  static constexpr size_t HEADER_BYTES = 62;
  static constexpr size_t MAX_ROW_BYTES = 256;

  void parseHeader();
  void commitRow();

  uint8_t* tile;
  uint8_t header[HEADER_BYTES];
  size_t headerLen = 0;
  bool headerDone = false;
  bool error = false;
  bool topDown = true;
  int width = 0;
  int height = 0;
  size_t rowBytes = 0;
  size_t dataOffset = HEADER_BYTES;
  size_t skipped = 0;
  uint8_t row[MAX_ROW_BYTES];
  size_t rowLen = 0;
  int rowsDone = 0;
  int cropX = 0;
  int cropY = 0;
};

// Decodes the JPEG at `jpegPath` into `tile` (COVER_TILE_BYTES). Uses the
// firmware's JPEG decoder with the same heap floor the Home screen applies.
bool renderCoverTile(const std::string& jpegPath, uint8_t* tile);

}  // namespace bookorbit
