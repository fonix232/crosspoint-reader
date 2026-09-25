#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <string>

#include "BookOrbitProgressPolicy.h"

// Bridges the reader's per-book progress.bin and BookOrbit's KOReader-style
// locators. The reader's saved position (spine + visible-text offset) is the
// durable local state. With `lendFramebuffer` the chapter streaming borrows
// the framebuffer as scratch (the low-heap path; callers must hold the
// rendering lock); without it the mapper allocates from the heap, which lets a
// background task on a PSRAM board run it while the screen stays live.
namespace bookorbit {

struct LocalProgressFile {
  bool valid = false;
  uint16_t spine = 0;
  uint16_t page = 0;
  uint16_t pageCount = 0;
  bool hasOffset = false;
  uint32_t offset = 0;

  LocalProgressState toState() const {
    LocalProgressState state;
    state.hasProgress = valid;
    state.spine = spine;
    state.key = localProgressKey(hasOffset, offset, page);
    return state;
  }
};

// Reads `<cache>/progress.bin` for the EPUB at `epubPath`. A missing file is
// not an error: `valid` stays false.
bool readLocalProgress(const std::string& epubPath, LocalProgressFile& out);

// KOReader locator and 0..1 percentage for a local position. Loads the book's
// metadata cache, building it when the book was never opened.
bool buildLocalLocator(const std::string& epubPath, const LocalProgressFile& local, GfxRenderer& renderer,
                       std::string& locator, float& percentage, bool lendFramebuffer = true);

// Maps a remote locator onto this book and writes it as the saved position.
// An unresolvable locator falls back to the percentage (approximate chapter
// start), which the reader treats like any other saved page.
bool applyRemoteLocator(const std::string& epubPath, const std::string& locator, float percentage,
                        GfxRenderer& renderer, LocalProgressFile& written, bool lendFramebuffer = true);

}  // namespace bookorbit
