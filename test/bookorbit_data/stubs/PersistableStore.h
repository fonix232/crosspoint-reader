#pragma once
#include <ArduinoJson.h>
#include <HalStorage.h>
class PersistableStoreBase {
 public:
  static bool readDocFromFile(const char* path, JsonDocument& doc) {
    const auto it = fake::files.find(path);
    return it != fake::files.end() && !deserializeJson(doc, it->second->bytes.data(), it->second->bytes.size());
  }
  static bool writeDocToFile(const char* path, const JsonDocument& doc) {
    std::string body;
    serializeJson(doc, body);
    HalFile file;
    return Storage.openFileForWrite("test", path, file) &&
           file.write(reinterpret_cast<const uint8_t*>(body.data()), body.size()) == body.size();
  }
};
