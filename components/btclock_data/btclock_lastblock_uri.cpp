#include "btclock_lastblock_uri.hpp"

namespace btclock {

std::string BuildLastblockUri(const std::string& ws_uri) {
  const char* http_scheme = nullptr;
  std::size_t scheme_len = 0;
  if (ws_uri.rfind("wss://", 0) == 0) {
    http_scheme = "https://";
    scheme_len = 6;
  } else if (ws_uri.rfind("ws://", 0) == 0) {
    http_scheme = "http://";
    scheme_len = 5;
  } else {
    return {};
  }

  // Authority = everything between the scheme and the first '/' (or end).
  // We deliberately drop the path — the user-configurable URI always
  // ends in /api/v2/ws today, but a future rename to /v3/ws shouldn't
  // route the lastblock probe at /api/v3/lastblock.
  const std::size_t authority_start = scheme_len;
  const std::size_t path_start = ws_uri.find('/', authority_start);
  const std::string authority =
      (path_start == std::string::npos)
          ? ws_uri.substr(authority_start)
          : ws_uri.substr(authority_start, path_start - authority_start);
  if (authority.empty()) return {};

  std::string out;
  out.reserve(scheme_len + authority.size() + 16);
  out.append(http_scheme);
  out.append(authority);
  out.append("/api/lastblock");
  return out;
}

}  // namespace btclock
