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
#include <vector>

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
  uint32_t frames_chunk = 0;
  uint32_t frames_complete = 0;
  uint32_t last_frame_bytes = 0;
  uint8_t last_evt_op_code = 0;
  uint8_t last_evt_fin = 0;
  int32_t last_evt_payload_offset = 0;
  int32_t last_evt_payload_len = 0;
  int32_t last_evt_data_len = 0;
  std::string last_emitted_head;  // first ≤ 96 bytes of last emitted frame

  // Ring buffer of the last ~16 raw WS event metadata records. Empty
  // when the relay is null. Lets /api/nwc/debug expose chunk-level
  // fragmentation evidence — chase mismatches between payload_offset
  // / payload_len / data_len here when the parser sees tail-only
  // buffers.
  struct WssEvt {
    uint32_t seq = 0;
    uint8_t op_code = 0;
    uint8_t fin = 0;
    uint8_t emit = 0;
    int32_t payload_offset = 0;
    int32_t payload_len = 0;
    int32_t data_len = 0;
  };
  std::vector<WssEvt> wss_evt_history;

  uint32_t reissue_count = 0;
  uint32_t parse_fail_count = 0;
  uint32_t event_dispatch_count = 0;
  std::string last_event_sub_id;
  std::string last_parse_fail_head;  // first ≤256 bytes of last rejected frame

  // Notification queue depth / counters — sampled from the live
  // NotificationQueue. Zero when no queue is wired (host tests, or
  // disabled NWC stack).
  uint32_t notif_queue_capacity = 0;
  uint32_t notif_queue_size = 0;
  uint32_t notif_queue_pushed = 0;
  uint32_t notif_queue_popped = 0;
  uint32_t notif_queue_dropped = 0;
};

// Render an NwcDebugInfo as a compact JSON string. Empty on OOM.
std::string BuildNwcDebugJson(const NwcDebugInfo& info);

}  // namespace nwc
}  // namespace btclock
