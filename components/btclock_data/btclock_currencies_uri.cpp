#include "btclock_currencies_uri.hpp"

namespace btclock {

std::string BuildCurrenciesUri(const std::string& ws_uri) {
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

  const std::size_t authority_start = scheme_len;
  const std::size_t path_start = ws_uri.find('/', authority_start);
  const std::string authority =
      (path_start == std::string::npos)
          ? ws_uri.substr(authority_start)
          : ws_uri.substr(authority_start, path_start - authority_start);
  if (authority.empty()) return {};

  std::string out;
  out.reserve(scheme_len + authority.size() + 24);
  out.append(http_scheme);
  out.append(authority);
  out.append("/api/v2/currencies");
  return out;
}

}  // namespace btclock
