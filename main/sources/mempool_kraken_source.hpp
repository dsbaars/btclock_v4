// mempool.space + Kraken WSS data source.
//
// Two independent WebSocket clients stuffed behind one DataSource:
//   * mempool.space (`wss://mempool.space/api/v1/ws`) — subscribes to
//     blocks / mempool-blocks / fees. Emits block_height on new blocks
//     and block_fee / block_fee_precise on fee ticks. The "next block
//     median fee" (legacy `blockfee2` parity) comes from the
//     `mempool-blocks` array — entry [0] is the next block.
//   * Kraken V2 (`wss://ws.kraken.com/v2`) — one ticker channel per
//     active currency in `actCurrencies`. Emits price.<ccy> as the last
//     trade price (Kraken's `last` field), formatted to a string.
//
// Both clients run independently — a kraken outage shouldn't take
// blocks down, and vice versa. Reuses esp_websocket_client's built-in
// reconnect (reconnect_timeout_ms=5000), same pattern as
// BtclockDataSource.
//
// Lives in main/sources/ (not its own component) because it pulls
// together pieces that already live in main: AppCtx wiring, currency
// list. The two WSS clients themselves are private members; if they
// grow callers outside this source, factor them out then.
//
// Selected when settings/dataSource == 1.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "data_core/source.hpp"
#include "esp_err.h"
#include "esp_websocket_client.h"

namespace btclock {

class MempoolKrakenSource : public DataSource {
 public:
  // `currencies` is the set of ISO codes to subscribe to ("USD","EUR",…).
  // `block_fee_dec` selects whether the precise-decimal fee field
  // (`block_fee_precise`) is populated alongside the rounded one. Mirrors
  // the BtClock v2 behaviour so renderers don't see a regression when
  // the user flips dataSource at runtime.
  MempoolKrakenSource(std::vector<std::string> currencies,
                      bool block_fee_dec);
  ~MempoolKrakenSource() override;

  const char* name() const override { return "mempool+kraken"; }
  esp_err_t Start(DataHub& hub) override;
  esp_err_t Stop() override;

  // Connection-state probes for /api/status. Mirror the
  // RelayClient::IsConnected pattern: WEBSOCKET_EVENT_CONNECTED toggles
  // the flag true, _DISCONNECTED/_ERROR toggles it false.
  bool IsMempoolConnected() const { return mempool_connected_.load(); }
  bool IsKrakenConnected() const { return kraken_connected_.load(); }

 private:
  static void MempoolEventTrampoline(void* arg, esp_event_base_t base,
                                     int32_t event_id, void* event_data);
  static void KrakenEventTrampoline(void* arg, esp_event_base_t base,
                                    int32_t event_id, void* event_data);
  void HandleMempoolEvent(int32_t event_id, void* event_data);
  void HandleKrakenEvent(int32_t event_id, void* event_data);

  // Frame parsers. Each accepts a NUL-terminated cJSON-parseable buffer
  // (the WS callback hands us a length, and we copy into a std::string
  // so cJSON can NUL-terminate). Both build a partial DataSnapshot and
  // forward to hub_->Report().
  void HandleMempoolFrame(const char* data, size_t len);
  void HandleKrakenFrame(const char* data, size_t len);

  // Subscribe frames are sent on each (re)connect — esp_websocket_client
  // doesn't preserve subscriptions across the disconnect, so the wire
  // protocol is "send subscribe immediately on _CONNECTED".
  void SendMempoolSubscriptions();
  void SendKrakenSubscriptions();

  std::vector<std::string> currencies_;
  bool block_fee_dec_ = true;

  DataHub* hub_ = nullptr;  // set in Start(); nulled in Stop()

  esp_websocket_client_handle_t mempool_client_ = nullptr;
  esp_websocket_client_handle_t kraken_client_ = nullptr;

  // Frame fragmentation buffers — both APIs send small JSON payloads,
  // but the websocket library can split them across WEBSOCKET_EVENT_DATA
  // events when the payload is larger than the rx buffer. We accumulate
  // until ev->payload_len == ev->payload_offset + ev->data_len and then
  // dispatch. Mirrors the pattern in old Arduino code (see v3 history).
  std::string mempool_frame_;
  std::string kraken_frame_;

  std::atomic<bool> mempool_connected_{false};
  std::atomic<bool> kraken_connected_{false};
};

}  // namespace btclock
