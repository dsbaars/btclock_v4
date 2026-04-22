// BTClock WS v2 data source.
//
// Connects to `wss://ws.btclock.dev/api/v2/ws` (or a user-configured
// URL), subscribes to blockheight / blockfee / price, and reports
// partial DataSnapshots to a DataHub. Implements the DataSource
// interface — the hub handles lifecycle and merging.
//
// Wire protocol is MessagePack. Frames are decoded with ArduinoJson.
// See docs/API.md in the btclock-ws-nostr-publish repo for the frame
// shapes.

#pragma once

#include <string>

#include "data_core/source.hpp"
#include "esp_err.h"
#include "esp_websocket_client.h"

namespace btclock {

class BtclockDataSource : public DataSource {
 public:
  explicit BtclockDataSource(const char* uri);
  ~BtclockDataSource() override;

  const char* name() const override { return "btclock-ws-v2"; }
  esp_err_t Start(DataHub& hub) override;
  esp_err_t Stop() override;

 private:
  static void EventHandlerTrampoline(void* arg, esp_event_base_t base,
                                     int32_t event_id, void* event_data);
  void HandleEvent(int32_t event_id, void* event_data);
  void HandleBinaryFrame(const uint8_t* data, size_t len);
  void SendSubscriptions();

  std::string uri_;
  DataHub* hub_ = nullptr;  // set in Start(); nulled in Stop()
  esp_websocket_client_handle_t client_ = nullptr;
};

}  // namespace btclock
