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
#include <vector>

#include "data_core/source.hpp"
#include "esp_err.h"
#include "esp_websocket_client.h"

namespace btclock {

class BtclockDataSource : public DataSource {
 public:
  // `currencies` is the set of ISO codes to subscribe to ("USD","EUR",…).
  // Empty = no price subscription (block + fee only).
  // `block_fee_dec` selects which fee stream the relay subscribes us to:
  //   true  -> "blockfee2" (2-decimal precise; fires every fee tick),
  //   false -> "blockfee"  (integer rounded; fires on rounded change).
  // Mirrors the `blockFeeDec` setting and is replaced live via
  // SetBlockFeeDec when the user PATCHes /api/settings.
  BtclockDataSource(const char* uri, std::vector<std::string> currencies,
                    bool block_fee_dec);
  ~BtclockDataSource() override;

  const char* name() const override { return "btclock-ws-v2"; }
  esp_err_t Start(DataHub& hub) override;
  esp_err_t Stop() override;

  // Replace the active currency subscription list at runtime. Called by
  // the on_screens_changed hook when PATCH /api/settings updates
  // `actCurrencies` so price ticks for newly-added codes start flowing
  // without a reboot. When the WS client is already running this stops
  // and restarts it so the server-side subscription state matches the
  // new list — additive `subscribe` frames alone wouldn't drop a
  // removed code's stream. The reconnect is ~5 s; cached snapshot
  // values keep the screens populated meanwhile. Empty input keeps the
  // existing list (a malformed PATCH shouldn't drop every subscription).
  void SetCurrencies(std::vector<std::string> currencies);

  // Replace the active fee-stream selection at runtime. Called by the
  // on_block_fee_dec_changed hook when PATCH /api/settings updates
  // `blockFeeDec`. Mirrors SetCurrencies — the WS is bounced so the
  // relay subscription set switches cleanly to exactly one of
  // {blockfee, blockfee2}, avoiding the both-subscribed double-dispatch
  // bug. Pre-Start callers (test harnesses) just update the field and
  // the next Start picks it up.
  void SetBlockFeeDec(bool block_fee_dec);

 private:
  static void EventHandlerTrampoline(void* arg, esp_event_base_t base,
                                     int32_t event_id, void* event_data);
  void HandleEvent(int32_t event_id, void* event_data);
  void HandleBinaryFrame(const uint8_t* data, size_t len);
  void SendSubscriptions();

  std::string uri_;
  std::vector<std::string> currencies_;
  // Mirrors the `blockFeeDec` pref. Subscribe path picks "blockfee2"
  // when true, "blockfee" when false; HandleBinaryFrame drops the
  // other one so a stale relay-side subscription can't smuggle the
  // wrong precision into the snapshot.
  bool block_fee_dec_ = true;
  DataHub* hub_ = nullptr;  // set in Start(); nulled in Stop()
  esp_websocket_client_handle_t client_ = nullptr;
};

}  // namespace btclock
