#pragma once
inline constexpr int WL_CONNECTED = 3;
struct FakeWiFi {
  int connection = 0;
  int status() const { return connection; }
};
inline FakeWiFi WiFi;
