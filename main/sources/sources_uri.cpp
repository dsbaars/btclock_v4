// Pure helper split out of sources.cpp so host tests can pin the URI
// shape without dragging in ESP-IDF / FreeRTOS / NVS. WireDataSources
// is the only firmware caller; everything else lives in test_host.

#include "sources/sources.hpp"

#include <cstdint>
#include <string>

namespace btclock {
namespace {
constexpr const char* kDefaultBtclockUri = "wss://ws.btclock.dev/api/v2/ws";
}  // namespace

std::string BuildBtclockSourceUri(std::uint8_t data_source,
                                  const std::string& endpoint,
                                  bool disable_ssl) {
  // dataSource enum mirrors btclock_v3_fci/src/lib/system/defaults.hpp:
  //   0 = BTCLOCK_SOURCE (the public ws.btclock.dev v2 feed)
  //   1 = mempool+kraken (separate source — see mempool_kraken_source.cpp;
  //       WireDataSources doesn't consult this helper for ds=1)
  //   2 = CUSTOM endpoint (user-supplied via ceEndpoint)
  //   3 = (legacy combo source, not implemented)
  // Anything other than 2 falls back to the public default; the caller
  // logs a warning so this is visible on the serial console rather than
  // silently masking a broken setting.
  if (data_source != 2) return kDefaultBtclockUri;

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
