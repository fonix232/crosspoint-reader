#pragma once

class GfxRenderer;
class HalDisplay;

namespace wired_debug {
// Called only from loop(), before mapped input update. Never used from a worker.
bool handleCommand(const char* command, GfxRenderer& renderer, HalDisplay& display, bool exclusiveStorage);
}  // namespace wired_debug
