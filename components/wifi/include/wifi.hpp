#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif_types.h"
#include "esp_timer.h"
#include "esp_wifi_types_generic.h"

namespace btclock {

struct WifiScanEntry {
  std::string ssid;
  int8_t rssi;
  bool secured;
};

// Minimal WiFi STA client. Non-blocking Connect; poll state() or call
// WaitConnected() from the caller. Caller owns reconnection policy via
// Disconnect + Connect; the class handles transient WiFi disconnect
// callbacks by auto-retrying every 2 s until stopped.
//
// Auto-retry is gated on an explicit sta_auto_retry_ flag (set by
// Connect, paused by TryConnect during a verify), NOT on whether the
// SoftAP is up. That decoupling is what lets the APSTA provisioning-
// fallback keep retrying the saved network while the portal broadcasts
// alongside it — see io/provisioning_fallback.hpp.
class Wifi {
 public:
  enum class State : uint8_t {
    kIdle,
    kConnecting,
    kConnected,
    kDisconnected,
  };

  Wifi();
  ~Wifi();

  Wifi(const Wifi&) = delete;
  Wifi& operator=(const Wifi&) = delete;

  // Initialise netif + event loop + wifi driver. Idempotent.
  esp_err_t Start();

  // Begin a STA connection to the given SSID. Returns immediately; state
  // transitions to kConnecting → kConnected (or back to kDisconnected on
  // timeout, with auto-retry after 5 s).
  esp_err_t Connect(const char* ssid, const char* password);

  // Block until state() == kConnected or timeout elapses.
  esp_err_t WaitConnected(uint32_t timeout_ms);

  // Synchronous credential-check: attempts one association with the given
  // creds and waits up to `timeout_ms` for either GOT_IP or a terminal
  // disconnect reason (AUTH_FAIL / NO_AP_FOUND / ASSOC_FAIL / HANDSHAKE /
  // CONNECTION_FAIL). Used by the provisioning portal to verify before
  // saving to NVS. Does NOT auto-retry. Caller should have SoftAP up so
  // the portal stays reachable for retries.
  //   ESP_OK                    — got IP (creds work).
  //   ESP_ERR_INVALID_RESPONSE  — terminal disconnect (wrong creds / wrong AP).
  //   ESP_ERR_TIMEOUT           — neither GOT_IP nor terminal reason within
  //                               the deadline.
  // Last reason is available via last_disconnect_reason() after return.
  esp_err_t TryConnect(const char* ssid, const char* password,
                       uint32_t timeout_ms);

  // Most recent STA disconnect reason code, or 0 before any disconnect.
  // Reset to 0 at the start of each Connect()/TryConnect() call.
  uint8_t last_disconnect_reason() const { return last_reason_.load(); }

  // Consecutive STA disconnects with a terminal reason (AUTH_FAIL=202,
  // NO_AP_FOUND=201, ASSOC_FAIL=203, HANDSHAKE_TIMEOUT=204,
  // CONNECTION_FAIL=205) since the last successful GOT_IP. Used by
  // wifi_guard to escalate back to provisioning after sustained failures.
  // Transient reasons (AUTH_EXPIRE, ASSOC_LEAVE, etc.) don't count.
  uint32_t consecutive_terminal_disconnects() const {
    return terminal_strikes_.load();
  }

  State state() const { return state_.load(); }
  // IPv4 as dotted-quad string, "0.0.0.0" while not connected.
  std::string ip() const;

  // --- SoftAP mode, for the provisioning portal ---

  // Switch from STA to SoftAP. `ssid` is the AP name the user sees.
  // If `password` is null or empty, the AP is open; otherwise WPA2-PSK
  // (min 8 chars required by the 802.11 standard).
  // AP IP defaults to 192.168.4.1 (the ESP-IDF default).
  esp_err_t StartSoftAp(const char* ssid, const char* password = nullptr);

  // Tear the SoftAP back down and return to STA-only (APSTA -> STA). The
  // STA association is preserved. Used by the provisioning-fallback
  // coordinator to drop the concurrent portal once STA reconnects to the
  // saved network. No-op if the AP is not up.
  esp_err_t StopSoftAp();

  // True while SoftAP mode is active.
  bool is_ap_mode() const { return ap_mode_.load(); }

  // IPv4 of the SoftAP interface (typically 192.168.4.1).
  std::string ap_ip() const;

  // Scan visible networks. Blocks until scan completes (~1-2 s). Valid
  // in both STA and SoftAP+STA modes. Sorted by RSSI, strongest first.
  std::vector<WifiScanEntry> Scan();

  // Kick off a non-blocking WiFi scan. Results land asynchronously in the
  // cache when WIFI_EVENT_SCAN_DONE fires; a periodic timer re-runs the
  // scan every `refresh_ms` ms (default 15 s) until StopBackgroundScan()
  // is called or the object is destroyed. Safe to call multiple times —
  // the timer is idempotent. Returns ESP_OK even if a scan is already
  // in flight (the request is simply skipped).
  esp_err_t StartBackgroundScan(uint32_t refresh_ms = 15'000);

  // Stop the periodic scan timer. Any in-flight scan still completes and
  // populates the cache.
  void StopBackgroundScan();

  // Returns the most recent cached scan results (populated by the
  // background scanner). Empty if no scan has completed yet.
  std::vector<WifiScanEntry> GetCachedScan() const;

  // True once at least one background scan has completed.
  bool scan_ready() const { return scan_ready_.load(); }

 private:
  static void EventTrampoline(void* arg, esp_event_base_t base,
                              int32_t event_id, void* event_data);
  void OnEvent(esp_event_base_t base, int32_t id, void* data);
  void OnScanDone();
  esp_err_t TriggerScanLocked();
  static void ScanTimerTrampoline(void* arg);
  // Build the STA wifi_config_t from creds and kick off an association.
  // Shared by Connect (which also records the persistent retry target)
  // and TryConnect (one-shot verify that must NOT clobber that target).
  esp_err_t ApplyStaConfigAndConnect(const char* ssid, const char* password);

  esp_netif_t* netif_sta_ = nullptr;
  esp_netif_t* netif_ap_ = nullptr;
  std::atomic<State> state_{State::kIdle};
  std::atomic<uint32_t> ip_ = 0;
  std::atomic<bool> ap_mode_{false};
  // Whether a STA disconnect should trigger a background auto-reconnect.
  // Set true by Connect, paused (false) by TryConnect for the duration of
  // a portal verify so the candidate attempt doesn't race the background
  // retry of the saved network. Independent of ap_mode_ so APSTA fallback
  // can broadcast the portal AND keep retrying the saved network.
  std::atomic<bool> sta_auto_retry_{false};
  std::atomic<uint8_t> last_reason_{0};
  std::atomic<uint32_t> terminal_strikes_{0};
  // Persistent STA target the background auto-retry reconnects to. Set by
  // Connect; TryConnect restores it after a failed verify so a recovered
  // saved network still auto-reconnects instead of the rejected candidate.
  // Main-task only (Connect/TryConnect) — OnEvent's retry just calls
  // esp_wifi_connect(), which reuses the driver's last set_config.
  std::string retry_ssid_;
  std::string retry_pw_;
  bool started_ = false;
  esp_event_handler_instance_t wifi_event_instance_ = nullptr;
  esp_event_handler_instance_t ip_event_instance_ = nullptr;

  // Background scanner state.
  mutable std::mutex scan_mu_;
  std::vector<WifiScanEntry> scan_cache_;  // guarded by scan_mu_
  std::atomic<bool> scan_in_flight_{false};
  std::atomic<bool> scan_ready_{false};
  esp_timer_handle_t scan_timer_ = nullptr;
};

}  // namespace btclock
