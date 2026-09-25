#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

#include "BookOrbitLibrarySelection.h"

/**
 * BookOrbit account configuration, persisted on the SD card.
 *
 * Independent of the KOReader sync store: a device may sync ordinary books to
 * a KOSync server and browse a BookOrbit library at the same time. The
 * password is XOR-obfuscated with the hardware MAC (see ObfuscationUtils),
 * which hides it from casual reading but is not encryption.
 */
class BookOrbitCredentialStore : public PersistableStore<BookOrbitCredentialStore> {
 private:
  std::string serverUrl;  // Origin plus optional path prefix, e.g. https://books.example.com
  std::string username;   // BookOrbit KOReader device username
  std::string password;   // BookOrbit KOReader device password
  std::string deviceId;   // Persistent random identity, generated on first use
  bookorbit::LibrarySelection libraries;
  bool insecureTls = false;
  bool syncProgress = true;
  uint8_t progressSyncMinutes = 30;  // background agent: periodic progress exchange, 0 = off

  BookOrbitCredentialStore() = default;
  ~BookOrbitCredentialStore() = default;

  friend class PersistableStore<BookOrbitCredentialStore>;

 public:
  static const char* getFilePath();
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const bookorbit::LibrarySelection& getLibraries() const { return libraries; }
  void setLibraries(const bookorbit::LibrarySelection& value) { libraries = value; }

  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const { return username; }
  const std::string& getPassword() const { return password; }
  std::string getMd5Password() const;
  void clearCredentials();

  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }
  // Normalized origin with any trailing "/api/v1" or slashes removed.
  std::string getOrigin() const;
  // `<origin>/api/v1`, the root of every BookOrbit device route.
  std::string getApiBase() const;
  bool usesHttps() const;

  // Server URL, username, and password are all present.
  bool isConfigured() const;

  void setInsecureTls(bool allow);
  bool getInsecureTls() const { return insecureTls; }

  void setSyncProgress(bool enabled);
  bool getSyncProgress() const { return syncProgress; }

  void setProgressSyncMinutes(uint8_t minutes) { progressSyncMinutes = minutes; }
  uint8_t getProgressSyncMinutes() const { return progressSyncMinutes; }

  // Random per-device identity sent with progress updates. Generated and
  // saved on first call so two CrossPoint devices on one account stay
  // distinguishable on the server.
  const std::string& getDeviceId();

  // Account namespace: 8 hex digits over origin + username. Directory names
  // under /.crosspoint/bookorbit/ are keyed by this.
  std::string getAccountKey() const;
  static std::string accountKeyFor(const std::string& origin, const std::string& username);
};

#define BOOKORBIT_STORE BookOrbitCredentialStore::getInstance()
