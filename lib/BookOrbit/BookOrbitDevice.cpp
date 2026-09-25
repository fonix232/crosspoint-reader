#include "BookOrbitDevice.h"

#include <BoardConfig.h>
#include <esp_mac.h>

#include <cstdio>

namespace bookorbit {

namespace {
// Model names follow the marketing names; FirmwareBoardTag keeps the short
// lowercase tags for release assets, which read worse in a device list.
constexpr const char* MODEL =
#if FREEINK_DEVICE_X4PRO
    "X4Pro";
#elif FREEINK_DEVICE_X4CLASSIC
    "X4C";
#elif FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
    "X4";
#elif FREEINK_DEVICE_PAPERS3
    "PaperS3";
#elif FREEINK_DEVICE_PAPERMONO
    "PaperMono";
#elif FREEINK_DEVICE_STICKY
    "Sticky";
#elif FREEINK_DEVICE_M5PAPER
    "M5Paper";
#elif FREEINK_DEVICE_LILYGO
    "T5S3";
#else
    "CrossPoint";
#endif
}  // namespace

const std::string& deviceName() {
  static std::string name;
  if (name.empty()) {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);  // factory base MAC; the Wi-Fi STA address derives from it
    char buf[32];
    snprintf(buf, sizeof(buf), "%s-%02x%02x%02x", MODEL, mac[3], mac[4], mac[5]);
    name = buf;
  }
  return name;
}

}  // namespace bookorbit
