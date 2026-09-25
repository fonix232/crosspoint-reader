#pragma once

#include <BoardConfig.h>

// Physical pixel density of the active panel, for layouts that size things in
// millimetres (the Library grid's cover width). The SDK profiles carry no
// diagonal, so the known panels are tabulated here from their published specs;
// an unknown board gets a typical e-paper density rather than a wrong one.
namespace screen_metrics {

inline constexpr int DEFAULT_PPI = 200;

inline int ppi() {
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC
  return 217;  // Xteink X4 family: 4.3" 800x480
#elif FREEINK_DEVICE_X3
  return 221;  // Xteink X3: 4.3" 792x528
#elif FREEINK_DEVICE_PAPERS3 || FREEINK_DEVICE_LILYGO
  return 234;  // ED047TC1: 4.7" 960x540
#elif FREEINK_DEVICE_STICKY
  return 124;  // Seeed reTerminal: 7.5" 800x480
#else
  return DEFAULT_PPI;
#endif
}

// Pixels for a physical length on the active panel.
inline int mmToPx(const int mm) { return (mm * ppi() * 10 + 127) / 254; }

// The Library grid's cover box: about 15 mm wide at a 2:3 ratio, clamped so
// unknown panels still get a sane layout. The BookOrbit sync requests
// server-rendered covers at exactly this size, so both must agree.
inline constexpr int GRID_COVER_MM = 15;
inline constexpr int GRID_COVER_MIN_PX = 56;
inline constexpr int GRID_COVER_MAX_PX = 240;
inline int gridCoverWidth() {
  int width = mmToPx(GRID_COVER_MM);
  if (width < GRID_COVER_MIN_PX) width = GRID_COVER_MIN_PX;
  if (width > GRID_COVER_MAX_PX) width = GRID_COVER_MAX_PX;
  return width;
}
inline int gridCoverHeight() { return gridCoverWidth() * 3 / 2; }

}  // namespace screen_metrics
