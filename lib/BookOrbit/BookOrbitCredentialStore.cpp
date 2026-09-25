#include "BookOrbitCredentialStore.h"

#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>
#include <esp_random.h>

#include <cstdio>

#include "BookOrbitPaths.h"

namespace {
constexpr uint8_t CONFIG_VERSION = 1;
constexpr char API_SUFFIX[] = "/api/v1";

uint32_t fnv1a(const std::string& text, uint32_t hash) {
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 16777619u;
  }
  return hash;
}
}  // namespace

const char* BookOrbitCredentialStore::getFilePath() { return bookorbit::CONFIG_PATH; }

void BookOrbitCredentialStore::toJson(JsonDocument& doc) const {
  doc["cfgVersion"] = CONFIG_VERSION;
  doc["serverUrl"] = serverUrl;
  doc["username"] = username;
  doc["password_obf"] = obfuscation::obfuscateToBase64(password);
  doc["deviceId"] = deviceId;
  doc["insecureTls"] = insecureTls;
  doc["syncProgress"] = syncProgress;
  doc["progressSyncMinutes"] = progressSyncMinutes;
  doc["allLibraries"] = libraries.all;
  auto ids = doc["libraryIds"].to<JsonArray>();
  for (unsigned i = 0; i < libraries.count; ++i) ids.add(libraries.ids[i]);
}

bool BookOrbitCredentialStore::fromJson(JsonVariantConst doc) {
  bool needsResave = false;
  const std::string pass = extractPassword(doc, needsResave);
  setCredentials(doc["username"] | "", pass);
  setServerUrl(doc["serverUrl"] | "");
  deviceId = doc["deviceId"] | "";
  insecureTls = doc["insecureTls"] | false;
  syncProgress = doc["syncProgress"] | true;
  progressSyncMinutes = doc["progressSyncMinutes"] | 30;
  libraries = bookorbit::LibrarySelection{};
  libraries.all = doc["allLibraries"] | true;
  if (!libraries.all) {
    for (JsonVariantConst entry : doc["libraryIds"].as<JsonArrayConst>()) {
      const uint32_t id = entry.is<uint32_t>() ? entry.as<uint32_t>() : 0;
      if (id && !libraries.contains(id)) libraries.toggle(id);
    }
  }
  if (needsResave) requestResave();
  return true;
}

void BookOrbitCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  if (username != user) libraries = bookorbit::LibrarySelection{};
  username = user;
  password = pass;
}

std::string BookOrbitCredentialStore::getMd5Password() const {
  if (password.empty()) return "";
  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();
  return md5.toString().c_str();
}

void BookOrbitCredentialStore::clearCredentials() {
  libraries = bookorbit::LibrarySelection{};
  username.clear();
  password.clear();
  saveToFile();
}

void BookOrbitCredentialStore::setServerUrl(const std::string& url) {
  const auto previous = getOrigin();
  serverUrl = url;
  if (previous != getOrigin()) libraries = bookorbit::LibrarySelection{};
}

std::string BookOrbitCredentialStore::getOrigin() const {
  std::string url = serverUrl;
  while (!url.empty() && (url.back() == ' ' || url.back() == '/')) url.pop_back();
  if (url.empty()) return url;
  if (url.find("://") == std::string::npos) url = "https://" + url;
  // Accept a pasted API base as well as the site origin.
  const size_t suffixLen = sizeof(API_SUFFIX) - 1;
  if (url.size() > suffixLen && url.compare(url.size() - suffixLen, suffixLen, API_SUFFIX) == 0) {
    url.erase(url.size() - suffixLen);
  }
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

std::string BookOrbitCredentialStore::getApiBase() const {
  const std::string origin = getOrigin();
  return origin.empty() ? origin : origin + API_SUFFIX;
}

bool BookOrbitCredentialStore::usesHttps() const { return getOrigin().rfind("https://", 0) == 0; }

bool BookOrbitCredentialStore::isConfigured() const {
  return !getOrigin().empty() && !username.empty() && !password.empty();
}

void BookOrbitCredentialStore::setInsecureTls(const bool allow) { insecureTls = allow; }

void BookOrbitCredentialStore::setSyncProgress(const bool enabled) { syncProgress = enabled; }

const std::string& BookOrbitCredentialStore::getDeviceId() {
  if (deviceId.empty()) {
    char buf[32];
    const uint32_t a = esp_random();
    const uint32_t b = esp_random();
    snprintf(buf, sizeof(buf), "crosspoint-%08lx%08lx", static_cast<unsigned long>(a), static_cast<unsigned long>(b));
    deviceId = buf;
    LOG_INF("BO", "Generated device id %s", deviceId.c_str());
    saveToFile();
  }
  return deviceId;
}

std::string BookOrbitCredentialStore::accountKeyFor(const std::string& origin, const std::string& user) {
  uint32_t hash = 2166136261u;
  hash = fnv1a(origin, hash);
  hash = fnv1a("\n", hash);
  hash = fnv1a(user, hash);
  char buf[12];
  snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hash));
  return buf;
}

std::string BookOrbitCredentialStore::getAccountKey() const { return accountKeyFor(getOrigin(), username); }
