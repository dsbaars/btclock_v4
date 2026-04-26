// Derive the device hostname from a user-set prefix and the Wi-Fi STA MAC.
// Kept header-only and ESP-IDF-free so both the on-device init path
// (main/app/boot/init_mdns.cpp) and the host-test suite can use it without
// linking esp_mac — callers read the MAC themselves and hand in six bytes.
//
// Shape: "<prefix>-<mac[3]><mac[4]><mac[5]>" (6 lowercase hex chars).
// This matches btclock_v3_fci's getMyHostname(), which is the string v3
// users already have bookmarked and what the WebUI mDNS scan helper looks
// for. Historically v4's /api/settings emitted a 4-char suffix instead —
// a drift that made the WebUI report a name nobody could actually ping.
// Route both call sites through this helper to keep them in sync.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace btclock {
namespace net_util {

// DNS-safe hostnames must be <=63 chars; the "-xxxxxx" suffix plus NUL
// leaves 56 for the user prefix. Matches the ceiling in init_mdns.cpp.
inline constexpr size_t kMaxHostnamePrefixLen = 56;

// Assemble the hostname. `prefix` is truncated to kMaxHostnamePrefixLen
// before concatenation; callers don't have to sanitise themselves.
inline std::string ComputeHostname(std::string_view prefix,
                                   const uint8_t mac[6]) {
  std::string clipped(prefix);
  if (clipped.size() > kMaxHostnamePrefixLen) {
    clipped.resize(kMaxHostnamePrefixLen);
  }
  char buf[kMaxHostnamePrefixLen + 1 /* dash */ + 6 /* mac */ + 1 /* NUL */];
  std::snprintf(buf, sizeof(buf), "%s-%02x%02x%02x", clipped.c_str(), mac[3],
                mac[4], mac[5]);
  return buf;
}

}  // namespace net_util
}  // namespace btclock
