#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace btclock {
namespace proxy {

enum class Kind : uint8_t {
  kNone = 0,
  kHttpConnect = 1,
  kSocks4 = 2,
  kSocks4a = 3,
  kSocks5 = 4,
};

struct Config {
  Kind kind = Kind::kNone;
  std::string host;
  uint16_t port = 0;
  std::string user;
  std::string pass;
  // Comma-separated globs from settings get split into this list.
  // Matching is case-insensitive; supported syntax is a literal host,
  // a leading-`*` glob ("*.local"), or a trailing-`*` glob ("192.168.*").
  std::vector<std::string> bypass;
};

inline bool IsEnabled(const Config& c) {
  return c.kind != Kind::kNone && !c.host.empty() && c.port != 0;
}

inline bool SupportsAuth(Kind k) {
  return k == Kind::kHttpConnect || k == Kind::kSocks5;
}

}  // namespace proxy
}  // namespace btclock
