#include "BookOrbitProgressSync.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ProgressFile.h>
#include <ProgressMapper.h>

#include <memory>
#include <optional>

namespace bookorbit {

namespace {
constexpr char TAG[] = "BOPRG";
constexpr char CACHE_ROOT[] = "/.crosspoint";

std::shared_ptr<Epub> loadEpub(const std::string& epubPath, const bool buildIfMissing) {
  auto owned = makeUniqueNoThrow<Epub>(epubPath, CACHE_ROOT);
  if (!owned) return nullptr;
  std::shared_ptr<Epub> epub(std::move(owned));
  epub->setupCacheDir();
  if (!epub->load(buildIfMissing, /*skipLoadingCss=*/true)) {
    LOG_ERR(TAG, "cannot load %s for progress mapping", epubPath.c_str());
    return nullptr;
  }
  return epub;
}

// Same 6/10-byte layout the reader writes (EpubReaderUtils::saveProgress).
bool writeLocalProgress(const Epub& epub, const LocalProgressFile& progress) {
  uint8_t data[10];
  data[0] = progress.spine & 0xFF;
  data[1] = (progress.spine >> 8) & 0xFF;
  data[2] = progress.page & 0xFF;
  data[3] = (progress.page >> 8) & 0xFF;
  data[4] = progress.pageCount & 0xFF;
  data[5] = (progress.pageCount >> 8) & 0xFF;
  size_t size = 6;
  if (progress.hasOffset) {
    data[6] = progress.offset & 0xFF;
    data[7] = (progress.offset >> 8) & 0xFF;
    data[8] = (progress.offset >> 16) & 0xFF;
    data[9] = (progress.offset >> 24) & 0xFF;
    size = sizeof(data);
  }
  return ProgressFile::writeAtomic(epub.getCachePath(), data, size);
}
}  // namespace

bool readLocalProgress(const std::string& epubPath, LocalProgressFile& out) {
  out = LocalProgressFile{};
  const Epub epub(epubPath, CACHE_ROOT);
  const std::string path = epub.getCachePath() + "/progress.bin";
  if (!Storage.exists(path.c_str())) return true;  // never opened
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return false;
  uint8_t data[10];
  const int size = f.read(data, sizeof(data));
  if (size != 4 && size != 6 && size != 10) {
    LOG_ERR(TAG, "unexpected progress.bin size %d for %s", size, epubPath.c_str());
    return false;
  }
  out.valid = true;
  out.spine = static_cast<uint16_t>(data[0] | (data[1] << 8));
  out.page = static_cast<uint16_t>(data[2] | (data[3] << 8));
  if (out.page == UINT16_MAX) out.page = 0;  // stale last-page sentinel
  if (size >= 6) out.pageCount = static_cast<uint16_t>(data[4] | (data[5] << 8));
  if (size == 10) {
    out.hasOffset = true;
    out.offset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                 (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
  }
  return true;
}

bool buildLocalLocator(const std::string& epubPath, const LocalProgressFile& local, GfxRenderer& renderer,
                       std::string& locator, float& percentage, const bool lendFramebuffer) {
  auto epub = loadEpub(epubPath, /*buildIfMissing=*/true);
  if (!epub) return false;
  CrossPointPosition pos{};
  pos.spineIndex = local.spine;
  pos.pageNumber = local.page;
  pos.totalPages = local.pageCount > 0 ? local.pageCount : 1;
  pos.hasVisibleTextOffset = local.hasOffset;
  pos.visibleTextOffset = local.offset;
  // No rendering may run while the chapter mapper borrows the framebuffer.
  std::optional<GfxRenderer::FrameBufferLoan> loan;
  if (lendFramebuffer) loan.emplace(renderer);
  const SavedProgressPosition saved = ProgressMapper::toSavedProgress(epub, pos);
  locator = saved.xpath;
  percentage = saved.percentage;
  return true;
}

bool applyRemoteLocator(const std::string& epubPath, const std::string& locator, const float percentage,
                        GfxRenderer& renderer, LocalProgressFile& written, const bool lendFramebuffer) {
  auto epub = loadEpub(epubPath, /*buildIfMissing=*/true);
  if (!epub) return false;
  CrossPointPosition mapped{};
  {
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (lendFramebuffer) loan.emplace(renderer);
    const SavedProgressPosition saved{locator, percentage};
    mapped = ProgressMapper::toCrossPoint(epub, saved, renderer);
  }
  written = LocalProgressFile{};
  written.valid = true;
  written.spine = static_cast<uint16_t>(mapped.spineIndex);
  written.page = static_cast<uint16_t>(mapped.pageNumber < 0 ? 0 : mapped.pageNumber);
  // A page count from an estimate would be treated as this layout's count;
  // leave it unknown so the reader repositions by content offset instead.
  written.pageCount = 0;
  written.hasOffset = mapped.hasVisibleTextOffset;
  written.offset = mapped.visibleTextOffset;
  if (!mapped.hasVisibleTextOffset) {
    LOG_INF(TAG, "remote locator not resolvable; applying approximate spine %d page %d", mapped.spineIndex,
            mapped.pageNumber);
  }
  return writeLocalProgress(*epub, written);
}

}  // namespace bookorbit
