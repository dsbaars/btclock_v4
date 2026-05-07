#pragma once

#include <functional>
#include <string>

#include "esp_err.h"
#include "esp_http_server.h"
#include "wifi.hpp"

namespace btclock {

// Tiny HTTP server that serves the provisioning portal:
//
//   GET /              → portal.html (cached, embedded in flash)
//   GET /api/scan      → JSON array of nearby networks
//   GET /api/version   → {"hw":"...","version":"...","built":"..."}
//   POST /api/wifi     → body "ssid=...&pw=...", save, schedule reboot
//   * (any other path) → portal.html 200 (CNA trigger; see HandleAny)
//
// The portal is designed for AP-only use (not reachable in STA mode).
class ProvisioningServer {
 public:
  using SaveCallback =
      std::function<void(const std::string& ssid, const std::string& pw)>;

  // `hw_name` is displayed in the portal footer (e.g. "Rev B", "V8").
  // Keep it short — the footer is narrow.
  ProvisioningServer(Wifi& wifi, const char* hw_name, SaveCallback on_save);
  ~ProvisioningServer();

  ProvisioningServer(const ProvisioningServer&) = delete;
  ProvisioningServer& operator=(const ProvisioningServer&) = delete;

  esp_err_t Start();

 private:
  static esp_err_t HandlePortal(httpd_req_t* req);
  static esp_err_t HandleScan(httpd_req_t* req);
  static esp_err_t HandleVersion(httpd_req_t* req);
  static esp_err_t HandleWifi(httpd_req_t* req);
  static esp_err_t HandleAny(httpd_req_t* req);

  Wifi* wifi_;
  const char* hw_name_;
  SaveCallback on_save_;
  httpd_handle_t server_ = nullptr;
};

// Trivial DNS hijack: binds UDP 53 and responds to every A query with
// `target_ip`. Runs on its own task; Stop() is fire-and-forget.
class DnsHijack {
 public:
  explicit DnsHijack(uint32_t target_ip);
  ~DnsHijack();
  esp_err_t Start();

 private:
  static void TaskTrampoline(void* arg);
  void Run();

  uint32_t target_ip_;
  int sock_ = -1;
  volatile bool stop_ = false;
  TaskHandle_t task_ = nullptr;
};

}  // namespace btclock
