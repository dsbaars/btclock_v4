// Single User-Agent string shared by every outbound WebSocket client
// (btclock-data, mempool, kraken-v2, nostr-relay). Centralising it keeps
// telemetry the upstreams see consistent and avoids drift when one call
// site is updated and the others aren't.
//
// Why a function-local static rather than the inline-variable shape used
// elsewhere: callers assign `cfg.user_agent = WebsocketUserAgent()` and
// the ESP-IDF WS client only copies the pointer's *target* lazily at
// handshake time, so the storage has to outlive client_init/start. A
// program-lifetime std::string makes that trivially safe and lets all
// four WS clients share one buffer.
//
// Sec-WebSocket-Key / Sec-WebSocket-Version are NOT set here — the WS
// client generates them per handshake (RFC 6455 requires a fresh random
// key each connect). Overriding them via cfg.headers would break the
// server's Sec-WebSocket-Accept check.

#pragma once

#include <string>

#include "esp_app_desc.h"

namespace btclock {
namespace net_util {

inline const char* WebsocketUserAgent() {
  static const std::string kUserAgent = [] {
    const char* rev = "unknown";
    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc != nullptr && desc->version[0] != '\0') rev = desc->version;
    const char* board =
#if defined(BTCLOCK_BOARD_REV_A)
        "rev-a";
#elif defined(BTCLOCK_BOARD_REV_B)
        "rev-b";
#elif defined(BTCLOCK_BOARD_V8)
        "v8";
#else
        "unknown";
#endif
    std::string s = "BTClock/";
    s += rev;
    s += " (esp32s3; ";
    s += board;
    s += ')';
    return s;
  }();
  return kUserAgent.c_str();
}

}  // namespace net_util
}  // namespace btclock
