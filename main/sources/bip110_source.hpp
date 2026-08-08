// BIP-110 chain tip source.
//
// kilombino is a hosted mempool.space instance, so it speaks the same
// `/api/v1/ws` protocol the mempool+kraken source already uses for the
// canonical chain: send one `{"action":"want","data":["blocks"]}` frame
// on connect, receive an ascending array of recent blocks as the initial
// snapshot, then a `block` (singular) push per new block. Frames are
// parsed with the same TipHeightFromBlocksArray helper, so both chains
// read their tip through identical code.
//
// Only the `blocks` topic is subscribed — no fees, no mempool-blocks.
// The dual block-height screen shows heights and nothing else, and the
// canonical chain's fee data already comes from the primary source.
//
// Result lands in DataSnapshot::bip110_block_height. The factory gates
// on the dual block-height screen (api_id 100) being visible, so a
// device that never displays the value doesn't hold a third WSS
// connection open against a third-party host.
//
// Lifecycle mirrors MempoolKrakenSource's mempool leg: an
// esp_websocket_client over an optional proxy transport chain, with the
// library's built-in 5 s reconnect and SafeShutdownWsClient on teardown.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "data_core/source.hpp"
#include "esp_err.h"
#include "esp_transport.h"
#include "esp_websocket_client.h"

namespace btclock {
namespace bip110 {

// Default upstream. Overridable via the `bip110Endpoint` pref (a full
// ws:// or wss:// URI) so the screen can be pointed at a self-hosted
// mempool instance without a rebuild.
inline constexpr const char* kDefaultUri =
    "wss://mempool.kilombino.com/api/v1/ws";

// Upper bound on a reassembled frame. The `blocks` snapshot is ~17 KB
// (eight full block objects including `extras`), so this is ~3x
// headroom; anything past it is a misbehaving or hostile upstream and
// gets dropped rather than grown into the heap.
inline constexpr std::size_t kMaxFrameBytes = 64 * 1024;

class Bip110Source : public DataSource {
 public:
  explicit Bip110Source(std::string uri);
  ~Bip110Source() override;

  const char* name() const override { return "bip110"; }
  esp_err_t Start(DataHub& hub) override;
  esp_err_t Stop() override;

  bool IsConnected() const { return connected_.load(); }

 private:
  static void EventTrampoline(void* arg, esp_event_base_t base,
                              int32_t event_id, void* event_data);
  void HandleEvent(int32_t event_id, void* event_data);
  void HandleFrame(const char* data, std::size_t len);
  void SendSubscription();

  std::string uri_;
  DataHub* hub_ = nullptr;

  esp_websocket_client_handle_t client_ = nullptr;
  esp_transport_handle_t proxy_inner_ = nullptr;
  esp_transport_handle_t proxy_ws_ = nullptr;

  // Fragment reassembly — the rx buffer is deliberately small, so the
  // initial snapshot always arrives split across several DATA events.
  std::string frame_;
  bool frame_overflow_ = false;

  std::atomic<bool> connected_{false};
};

// Factory — returns nullptr when the dual block-height screen is hidden
// or the configured URI is empty.
std::unique_ptr<DataSource> MakeBip110Source();

}  // namespace bip110
}  // namespace btclock
