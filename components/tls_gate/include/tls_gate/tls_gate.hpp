// Single lock that serialises every TLS handshake across the firmware.
//
// Each in-flight mbedtls SSL context holds ~16 KB of IN buffer plus
// scratch space; on Rev B the DRAM pool is ~350 KB shared across Wi-Fi,
// LWIP, display, data-source WS clients, HTTPS pollers and the WebUI
// async server. With THIRD_PARTY_SOURCE (mempool WS + Kraken WS) plus
// Nostr WS plus bitaxe HTTPS plus mining-pool HTTPS all starting up at
// boot, the moment when three or four handshakes overlap is the moment
// heap briefly dips below what a fresh mbedtls session needs. The
// second handshake then fails to alloc, surfaces as
// `Error retrieving X. HTTP status code: -1` or a WS stuck at
// "Connection Closed".
//
// Taking this mutex around the narrow window in which a TLS handshake
// is actually happening — i.e. the HTTP client GET/POST for HTTPS
// pollers, and the initial WS connect for WebSocket clients when they
// are NOT already connected — forces those handshakes to happen
// sequentially. Peak heap pressure drops from "four contexts alive at
// once" to "one", while the steady-state fast path (an already-open
// connection) stays contention-free.
//
// Usage (ESP-IDF / C++17):
//
//   #include "tls_gate/tls_gate.hpp"
//   {
//     std::lock_guard<std::mutex> lk(btclock::tls_gate::mutex());
//     // ... TLS handshake window only (connect/GET), NOT the whole
//     //     request lifetime and NOT steady-state reads/writes.
//   }
//
// The gate is header-only — it is a single function-local static
// std::mutex, so there is exactly one instance process-wide regardless
// of how many translation units include this header.

#pragma once

#include <mutex>

namespace btclock {
namespace tls_gate {

inline std::mutex& mutex() {
  static std::mutex m;
  return m;
}

}  // namespace tls_gate
}  // namespace btclock
