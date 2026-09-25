#pragma once

#include <HalStorage.h>

#include <string>

namespace bookorbit::storage {

// Keep the previous file recoverable across either rename in a staged replacement.
inline bool recover(const std::string& path) {
  if (Storage.exists(path.c_str())) return true;
  const std::string backup = path + ".bak";
  return !Storage.exists(backup.c_str()) || Storage.rename(backup.c_str(), path.c_str());
}

inline bool replace(const std::string& staging, const std::string& path) {
  if (!recover(path)) return false;
  const std::string backup = path + ".bak";
  if (Storage.exists(backup.c_str()) && !Storage.remove(backup.c_str())) return false;
  const bool previous = Storage.exists(path.c_str());
  if (previous && !Storage.rename(path.c_str(), backup.c_str())) return false;
  if (!Storage.rename(staging.c_str(), path.c_str())) {
    if (previous) Storage.rename(backup.c_str(), path.c_str());
    return false;
  }
  if (previous) Storage.remove(backup.c_str());
  return true;
}

}  // namespace bookorbit::storage
