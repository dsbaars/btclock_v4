// Diagnostic JSON snapshot for /api/nwc/debug.
//
// Aggregates NwcClient counters with the underlying RelayClient and
// SubscriptionManager state so the device can answer
// "which segment of the kind-23197 path swallowed the notification?"
// without a serial console. Kept in a dedicated TU (instead of the
// webserver layer) so the JSON builder is host-testable — both sides
// of the device-vs-host firewall can build the same string.
//
// Threading: callers must collect every input on the relay-worker
// task or via atomic loads. The builder itself is pure-logic.

#pragma once

#include <cstdint>
#include <string>

#include "nwc/client.hpp"

namespace btclock {
namespace nwc {

// Aggregated snapshot — populated by the HTTP handler from
// NwcClient::GetDebugSnapshot() plus the RelayClient /
// SubscriptionManager accessors.
struct NwcDebugInfo {
  // True iff InitNwc successfully wired the stack. When false the
  // JSON payload still emits the zero-initialised counters so the
  // WebUI / curl-loop sees a stable shape.
  bool enabled = false;

  DebugSnapshot client{};

  std::string wss_url;
  bool wss_connected = false;
  uint32_t reconnect_count = 0;
  int64_t last_connect_ms = 0;
  int64_t last_disconnect_ms = 0;

  uint32_t reissue_count = 0;
};

// Render an NwcDebugInfo as a compact JSON string. Empty on OOM.
std::string BuildNwcDebugJson(const NwcDebugInfo& info);

}  // namespace nwc
}  // namespace btclock
