#include "wifi.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace btclock {
namespace {
constexpr const char* kTag = "wifi";

std::string Ipv4ToString(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                static_cast<unsigned>(v & 0xFF),
                static_cast<unsigned>((v >> 8) & 0xFF),
                static_cast<unsigned>((v >> 16) & 0xFF),
                static_cast<unsigned>((v >> 24) & 0xFF));
  return buf;
}
}  // namespace

Wifi::Wifi() = default;

Wifi::~Wifi() {
  if (wifi_event_instance_) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          wifi_event_instance_);
  }
  if (ip_event_instance_) {
    esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID,
                                          ip_event_instance_);
  }
  if (started_) {
    esp_wifi_stop();
    esp_wifi_deinit();
  }
  if (netif_sta_) esp_netif_destroy_default_wifi(netif_sta_);
  if (netif_ap_) esp_netif_destroy_default_wifi(netif_ap_);
}

esp_err_t Wifi::Start() {
  if (started_) return ESP_OK;

  ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "netif_init");
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kTag, "event_loop_create_default: %s", esp_err_to_name(err));
    return err;
  }

  netif_sta_ = esp_netif_create_default_wifi_sta();
  if (!netif_sta_) return ESP_FAIL;

  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), kTag, "wifi_init");

  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                          WIFI_EVENT, ESP_EVENT_ANY_ID, EventTrampoline,
                          this, &wifi_event_instance_),
                      kTag, "reg wifi evt");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                          IP_EVENT, ESP_EVENT_ANY_ID, EventTrampoline, this,
                          &ip_event_instance_),
                      kTag, "reg ip evt");

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "set_mode");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi_start");
  started_ = true;
  ESP_LOGI(kTag, "wifi stack up, STA mode");
  return ESP_OK;
}

esp_err_t Wifi::Connect(const char* ssid, const char* password) {
  if (!started_) ESP_RETURN_ON_ERROR(Start(), kTag, "auto-start");

  wifi_config_t cfg = {};
  std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid,
               sizeof(cfg.sta.ssid) - 1);
  if (password && *password) {
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), password,
                 sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
  }
  cfg.sta.pmf_cfg.capable = true;

  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), kTag,
                      "set_config");
  state_.store(State::kConnecting);
  esp_wifi_connect();
  ESP_LOGI(kTag, "connecting to '%s'", ssid);
  return ESP_OK;
}

esp_err_t Wifi::WaitConnected(uint32_t timeout_ms) {
  const TickType_t deadline =
      xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  while (state_.load() != State::kConnected) {
    if (xTaskGetTickCount() >= deadline) return ESP_ERR_TIMEOUT;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return ESP_OK;
}

std::string Wifi::ip() const { return Ipv4ToString(ip_.load()); }

std::string Wifi::ap_ip() const {
  if (!netif_ap_) return "0.0.0.0";
  esp_netif_ip_info_t info = {};
  esp_netif_get_ip_info(netif_ap_, &info);
  return Ipv4ToString(info.ip.addr);
}

esp_err_t Wifi::StartSoftAp(const char* ssid, const char* password) {
  // Bring up netif + event loop + wifi driver if we haven't already.
  ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "netif_init");
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kTag, "event_loop_create_default: %s", esp_err_to_name(err));
    return err;
  }

  if (!netif_ap_) netif_ap_ = esp_netif_create_default_wifi_ap();
  if (!netif_ap_) return ESP_FAIL;

  if (!started_) {
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), kTag, "wifi_init.ap");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, EventTrampoline,
                            this, &wifi_event_instance_),
                        kTag, "reg wifi evt ap");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, ESP_EVENT_ANY_ID, EventTrampoline,
                            this, &ip_event_instance_),
                        kTag, "reg ip evt ap");
  }

  wifi_config_t ap_cfg = {};
  std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), ssid,
               sizeof(ap_cfg.ap.ssid) - 1);
  ap_cfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
  ap_cfg.ap.channel = 1;
  ap_cfg.ap.max_connection = 4;
  ap_cfg.ap.beacon_interval = 100;
  if (password && std::strlen(password) >= 8) {
    std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.password), password,
                 sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;   // compatibility with WPA2-only clients
  } else {
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
  }

  // Run AP+STA so the caller can scan with the STA radio while the AP
  // serves the portal.
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kTag, "set APSTA");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), kTag,
                      "set AP cfg");

  if (!started_) {
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi_start.ap");
    started_ = true;
  }
  ap_mode_.store(true);
  ESP_LOGI(kTag, "SoftAP up ssid='%s' auth=%s ip=%s", ssid,
           ap_cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2-PSK",
           ap_ip().c_str());
  return ESP_OK;
}

std::vector<WifiScanEntry> Wifi::Scan() {
  std::vector<WifiScanEntry> out;
  if (!started_) return out;

  wifi_scan_config_t cfg = {};
  cfg.show_hidden = false;
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  cfg.scan_time.active.min = 120;
  cfg.scan_time.active.max = 180;
  esp_err_t err = esp_wifi_scan_start(&cfg, /*block=*/true);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "scan_start err=%s", esp_err_to_name(err));
    return out;
  }

  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count == 0) return out;
  if (ap_count > 32) ap_count = 32;
  std::vector<wifi_ap_record_t> records(ap_count);
  esp_wifi_scan_get_ap_records(&ap_count, records.data());

  out.reserve(ap_count);
  for (uint16_t i = 0; i < ap_count; ++i) {
    WifiScanEntry e;
    e.ssid.assign(reinterpret_cast<const char*>(records[i].ssid));
    if (e.ssid.empty()) continue;
    e.rssi = records[i].rssi;
    e.secured = records[i].authmode != WIFI_AUTH_OPEN;
    out.push_back(std::move(e));
  }
  // De-duplicate by SSID (keep strongest) and sort by RSSI desc.
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.rssi > b.rssi; });
  std::vector<WifiScanEntry> uniq;
  for (auto& e : out) {
    bool seen = false;
    for (auto& u : uniq) if (u.ssid == e.ssid) { seen = true; break; }
    if (!seen) uniq.push_back(std::move(e));
  }
  return uniq;
}

void Wifi::EventTrampoline(void* arg, esp_event_base_t base, int32_t id,
                           void* data) {
  static_cast<Wifi*>(arg)->OnEvent(base, id, data);
}

void Wifi::OnEvent(esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
    ESP_LOGW(kTag, "disconnected reason=%u", ev ? ev->reason : 0);
    state_.store(State::kDisconnected);
    ip_.store(0);
    // Only auto-retry if we were actually trying to be a STA (not in
    // provisioning-AP mode and not explicitly idle).
    if (!ap_mode_.load()) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      state_.store(State::kConnecting);
      esp_wifi_connect();
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* ev = static_cast<ip_event_got_ip_t*>(data);
    ip_.store(ev->ip_info.ip.addr);
    state_.store(State::kConnected);
    ESP_LOGI(kTag, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
    const auto* ev = static_cast<wifi_event_ap_staconnected_t*>(data);
    ESP_LOGI(kTag, "AP client connected %02x:%02x:%02x:%02x:%02x:%02x",
             ev->mac[0], ev->mac[1], ev->mac[2], ev->mac[3], ev->mac[4],
             ev->mac[5]);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
    const auto* ev = static_cast<wifi_event_ap_stadisconnected_t*>(data);
    ESP_LOGI(kTag, "AP client disconnected %02x:%02x:%02x:%02x:%02x:%02x",
             ev->mac[0], ev->mac[1], ev->mac[2], ev->mac[3], ev->mac[4],
             ev->mac[5]);
  }
}

}  // namespace btclock
