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

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "data_core/source.hpp"
#include "esp_err.h"
#include "esp_transport.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

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

  // True iff WEBSOCKET_EVENT_CONNECTED fired more recently than
  // WEBSOCKET_EVENT_DISCONNECTED. Set/cleared by HandleEvent; survives
  // SetCurrencies/SetBlockFeeDec bounces because Stop() clears it on
  // the way out and Start()'s CONNECTED event re-sets it. Used by
  // ControlServer's `cfg_.v2_connected` callback on dataSource=0 so
  // /api/status reflects real WS-layer liveness instead of the
  // pre-fix "hub != nullptr" heuristic (bd btclock_v4-1xc / -lfd).
  bool IsConnected() const { return ws_connected_.load(); }

  // Lifecycle state: true between Start() and Stop(). Distinct from
  // IsConnected() — the latter reflects the live WS handshake while
  // this one captures "this source is running by design". Consumers
  // that want a fault signal (e.g. NetworkLedWatchdog) care about
  // "running but disconnected", not "stopped on purpose". Without this
  // distinction every intentional Stop() (OTA quiesce,
  // /api/stop_datasources, factory reset, dataSource flip) trips a red
  // breath over whatever the user is actually trying to do.
  bool IsActive() const { return client_ != nullptr; }

  // Install a callback that requests a hub-level Stop+Start when the
  // staleness watchdog confirms the WS is wedged (blockheight hasn't
  // changed in 60+ min AND upstream tip disagrees). The expected
  // implementation posts a `ControlCommand::Kind::kRestartDataSources`
  // to the main task so the lifecycle stays single-threaded — see
  // bd btclock_v4-lfd. Pre-fix the watchdog called
  // `esp_websocket_client_close()` directly from the probe task; on a
  // silently-wedged WS the lib treated that as a no-op and the device
  // stayed stale until operator intervention. Empty callback falls back
  // to the close-only behaviour (kept as defense-in-depth — the
  // close+reconnect path still works for the common TCP-level drop
  // case, just not for the silent-subscription-drop case the bug
  // is about).
  void SetRestartRequester(std::function<void()> cb) {
    restart_requester_ = std::move(cb);
  }

 private:
  static void EventHandlerTrampoline(void* arg, esp_event_base_t base,
                                     int32_t event_id, void* event_data);
  void HandleEvent(int32_t event_id, void* event_data);
  void HandleBinaryFrame(const uint8_t* data, size_t len);
  void SendSubscriptions();

  // Stale-block watchdog. The websocket's own keepalive (ping/pong +
  // linear reconnect) catches a TCP-level failure within ~35 s, but
  // there's a class of "WS thinks it's healthy, server stopped
  // forwarding blockheight updates" failures that the keepalive misses
  // entirely (relay-side subscription dropped, NAT rebind on the AP,
  // server-side WS write blocked). The watchdog is the safety net for
  // those: every ~5 min we check how long since the snapshot's
  // blockheight last *changed*; if it's been longer than the configured
  // stale window (60 min today) we GET /api/lastblock over HTTP and,
  // when the upstream tip disagrees with our cached value, force a
  // close+reconnect on the WS so SendSubscriptions() replays.
  static void WatchdogTrampoline(TimerHandle_t timer);
  void OnWatchdogTick();
  // Worker entrypoint for the staleness probe + reconnect. Runs on a
  // transient task (NOT on Tmr Svc) so the TLS HTTPS handshake inside
  // FetchUpstreamHeight has the stack it needs. Spawned only when the
  // staleness threshold is exceeded; self-deletes on completion.
  static void ProbeTaskTrampoline(void* arg);
  void RunStalenessProbe();
  // Probes `/api/lastblock`; returns true and sets `out` on HTTP 2xx
  // with a parseable integer body, false otherwise. False means
  // "couldn't probe" — the watchdog treats that as graceful and tries
  // again on the next tick, deliberately *not* triggering a reconnect.
  bool FetchUpstreamHeight(uint32_t& out) const;
  // Closes the underlying WS so the esp_websocket_client built-in
  // reconnect kicks in. We deliberately don't Stop()+Start() here —
  // close-only preserves event-handler registration, lets the
  // CONNECTED handler replay subscriptions on its own, and avoids
  // racing the watchdog timer's lifecycle against the client handle.
  void ForceReconnect();

  std::string uri_;
  std::vector<std::string> currencies_;
  // Mirrors the `blockFeeDec` pref. Subscribe path picks "blockfee2"
  // when true, "blockfee" when false; HandleBinaryFrame drops the
  // other one so a stale relay-side subscription can't smuggle the
  // wrong precision into the snapshot.
  bool block_fee_dec_ = true;
  DataHub* hub_ = nullptr;  // set in Start(); nulled in Stop()
  esp_websocket_client_handle_t client_ = nullptr;
  // Serializes the whole Start()/Stop() lifecycle. Stop() is reachable
  // from multiple tasks — the main event loop (SetSubscriptions /
  // kStopDataSources / kRestartDataSources), the OTA-quiesce worker
  // (DataHub::StopAll), and the destructor. Without this, two
  // concurrent Stop()s race on the non-atomic `client_` capture-and-
  // null and both call SafeShutdownWsClient on the same handle: the
  // second esp_websocket_client_destroy() deletes the status_bits
  // event group out from under the first's esp_websocket_client_stop(),
  // whose portMAX_DELAY wait for STOPPED_BIT then never returns —
  // wedging the main loop forever. The ws task (which stop() joins) and
  // the watchdog/probe paths (serialized via probe_in_flight_) never
  // take this lock, so holding it across the blocking teardown can't
  // deadlock.
  std::mutex lifecycle_mu_;
  // Proxy chain handed to client_ via cfg.ext_transport. The WS client
  // doesn't own ext_transport (esp_websocket_client.c skips its
  // transport_list build when ext_transport is set), so we destroy
  // both handles in Stop() in reverse-construction order.
  esp_transport_handle_t proxy_inner_ = nullptr;
  esp_transport_handle_t proxy_ws_ = nullptr;

  // Watchdog state. `last_height_` tracks the most recent value we
  // accepted off the wire; we only bump `last_change_tick_` when the
  // value actually *changes*, so a relay re-broadcasting the current
  // tip can't fool the staleness check. Ticks come from
  // xTaskGetTickCount(); zero is treated as "no sample yet".
  TimerHandle_t watchdog_timer_ = nullptr;
  std::atomic<uint32_t> last_height_{0};
  std::atomic<TickType_t> last_change_tick_{0};
  // Set true while a probe worker task is running. Tmr Svc uses this
  // to skip spawning a second probe when one is still in flight, and
  // Stop() polls it before destroying the WS client so the worker
  // can't dereference a destroyed handle.
  std::atomic<bool> probe_in_flight_{false};
  // WS-layer connection state, driven by the CONNECTED/DISCONNECTED
  // events on the client. NOT the same as data freshness — silent
  // subscription drops leave this `true` while blockheight stops
  // updating; that case is what the staleness watchdog handles. See
  // IsConnected().
  std::atomic<bool> ws_connected_{false};
  // Optional hub-level restart hook. When set (wired in
  // init_control_api.cpp once ControlServer exists), the staleness
  // watchdog uses it instead of `esp_websocket_client_close()`. Null
  // by default so test harnesses without a ControlServer fall through
  // to ForceReconnect(). See SetRestartRequester().
  std::function<void()> restart_requester_;
};

}  // namespace btclock
