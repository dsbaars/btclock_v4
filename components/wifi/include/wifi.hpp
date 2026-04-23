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
// callbacks by auto-retrying every 5 s until stopped.
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

  State state() const { return state_.load(); }
  // IPv4 as dotted-quad string, "0.0.0.0" while not connected.
  std::string ip() const;

  // --- SoftAP mode, for the provisioning portal ---

  // Switch from STA to SoftAP. `ssid` is the AP name the user sees.
  // If `password` is null or empty, the AP is open; otherwise WPA2-PSK
  // (min 8 chars required by the 802.11 standard).
  // AP IP defaults to 192.168.4.1 (the ESP-IDF default).
  esp_err_t StartSoftAp(const char* ssid, const char* password = nullptr);

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

  esp_netif_t* netif_sta_ = nullptr;
  esp_netif_t* netif_ap_ = nullptr;
  std::atomic<State> state_{State::kIdle};
  std::atomic<uint32_t> ip_ = 0;
  std::atomic<bool> ap_mode_{false};
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
