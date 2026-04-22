#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif_types.h"
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

 private:
  static void EventTrampoline(void* arg, esp_event_base_t base,
                              int32_t event_id, void* event_data);
  void OnEvent(esp_event_base_t base, int32_t id, void* data);

  esp_netif_t* netif_sta_ = nullptr;
  esp_netif_t* netif_ap_ = nullptr;
  std::atomic<State> state_{State::kIdle};
  std::atomic<uint32_t> ip_ = 0;
  std::atomic<bool> ap_mode_{false};
  bool started_ = false;
  esp_event_handler_instance_t wifi_event_instance_ = nullptr;
  esp_event_handler_instance_t ip_event_instance_ = nullptr;
};

}  // namespace btclock
