// Pure helper split out of sources.cpp so host tests can pin the URI
// shape without dragging in ESP-IDF / FreeRTOS / NVS. WireDataSources
// is the only firmware caller; everything else lives in test_host.

#include <cstdint>
#include <string>

#include "sources/sources.hpp"

namespace btclock {
namespace {
constexpr const char* kDefaultBtclockUri = "wss://ws.btclock.dev/api/v2/ws";
}  // namespace

std::string BuildBtclockSourceUri(std::uint8_t data_source,
                                  const std::string& endpoint,
                                  bool disable_ssl) {
  // dataSource enum is shared with the WebUI (data/src/lib/types/settings.ts
  // DataSourceType):
  //   0 = BTCLOCK_SOURCE        — public ws.btclock.dev v2 feed
  //   1 = THIRD_PARTY_SOURCE    — mempool+kraken, handled separately in
  //                                WireDataSources (this helper isn't
  //                                consulted on that branch).
  //   2 = NOSTR_SOURCE          — Nostr is enabled via its own settings
  //                                (nostrEnable / nostrRelay / nostrPubKey).
  //                                For the BTClock data-source URI we
  //                                still consult ceEndpoint here so a
  //                                user running Nostr alongside a custom
  //                                price feed gets the custom feed; falls
  //                                back to the public default when the
  //                                endpoint is empty.
  //   3 = CUSTOM_SOURCE         — user-supplied custom endpoint via
  //                                ceEndpoint. Same branch as 2; the
  //                                two values share the custom path so
  //                                legacy NVS state (which used 2 for
  //                                "custom" before the WebUI added the
  //                                Nostr enum value) keeps working.
  // Anything other than {2, 3} falls back to the public default.
  if (data_source != 2 && data_source != 3) return kDefaultBtclockUri;

  // Defensive scheme-strip: users routinely paste a full URL into the
  // "Custom Endpoint" textbox. We tolerate that and rebuild the scheme
  // from ceDisableSSL so the two settings can't disagree.
  std::string host = endpoint;
  if (host.rfind("wss://", 0) == 0) {
    host.erase(0, 6);
  } else if (host.rfind("ws://", 0) == 0) {
    host.erase(0, 5);
  }
  if (host.empty()) return kDefaultBtclockUri;

  std::string out;
  out.reserve(host.size() + 18);
  out.append(disable_ssl ? "ws://" : "wss://");
  out.append(host);
  out.append("/api/v2/ws");
  return out;
}

}  // namespace btclock
