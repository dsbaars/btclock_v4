#pragma once

// Tiny URL helpers — kept in a header free of IDF includes so the
// host-test suite can use them without pulling esp_transport.h.

#include <string>
#include <string_view>

namespace btclock {
namespace proxy {

// True when the URL/URI scheme is one of "https://" or "wss://".
inline bool UrlImpliesTls(std::string_view url) {
  return (url.size() >= 8 && url.substr(0, 8) == "https://") ||
         (url.size() >= 6 && url.substr(0, 6) == "wss://");
}

// Extract the path portion of a URI, defaulting to "/" when no path
// component is present. Used by the WS migration to pass the path to
// esp_transport_ws_set_config — when ext_transport is set the WS
// client skips its internal apply (esp_websocket_client.c:582,647)
// so we have to push the path on ourselves.
//
// Handles only what the current call sites need:
// `scheme://host[:port]/path[?query]`. Anchors, IPv6 zone IDs, and
// userinfo aren't expected — the device never builds those URIs.
inline std::string PathFromUri(std::string_view uri) {
  const size_t scheme_end = uri.find("://");
  const size_t host_start =
      (scheme_end == std::string_view::npos) ? 0 : (scheme_end + 3);
  const size_t slash = uri.find('/', host_start);
  if (slash == std::string_view::npos) return "/";
  return std::string(uri.substr(slash));
}

}  // namespace proxy
}  // namespace btclock
