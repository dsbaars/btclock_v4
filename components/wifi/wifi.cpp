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
  StopBackgroundScan();
  if (scan_timer_) {
    esp_timer_delete(scan_timer_);
    scan_timer_ = nullptr;
  }
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
                          WIFI_EVENT, ESP_EVENT_ANY_ID, EventTrampoline, this,
                          &wifi_event_instance_),
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

esp_err_t Wifi::ApplyStaConfigAndConnect(const char* ssid,
                                         const char* password) {
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
  // Fresh attempt — callers use last_disconnect_reason() to tell "still
  // connecting" from "just failed," which requires a 0 baseline here.
  last_reason_.store(0);
  state_.store(State::kConnecting);
  esp_wifi_connect();
  ESP_LOGI(kTag, "connecting to '%s'", ssid);
  return ESP_OK;
}

esp_err_t Wifi::Connect(const char* ssid, const char* password) {
  if (!started_) ESP_RETURN_ON_ERROR(Start(), kTag, "auto-start");

  // Record the persistent retry target so a TryConnect verify can restore
  // it afterwards, then arm the background auto-reconnect.
  retry_ssid_ = ssid ? ssid : "";
  retry_pw_ = password ? password : "";
  sta_auto_retry_.store(true);
  return ApplyStaConfigAndConnect(ssid, password);
}

esp_err_t Wifi::TryConnect(const char* ssid, const char* password,
                           uint32_t timeout_ms) {
  if (!started_) ESP_RETURN_ON_ERROR(Start(), kTag, "try_connect.start");

  // Pause the background auto-reconnect for the duration of the verify so
  // the candidate attempt doesn't race a retry of the saved network on
  // the shared radio. We drive the candidate association directly (NOT via
  // Connect) so retry_ssid_/retry_pw_ keep pointing at the saved network.
  const bool had_retry_target = !retry_ssid_.empty();
  sta_auto_retry_.store(false);
  ESP_RETURN_ON_ERROR(ApplyStaConfigAndConnect(ssid, password), kTag,
                      "try_connect");

  // Restore the saved-network association and re-arm background retry —
  // used on every non-success exit so a recovered saved network still
  // auto-reconnects instead of the rejected candidate. No-op when there
  // was no saved target (pure-provisioning boot with empty creds).
  auto resume_saved = [this, had_retry_target]() {
    esp_wifi_disconnect();
    if (had_retry_target) {
      sta_auto_retry_.store(true);
      ApplyStaConfigAndConnect(retry_ssid_.c_str(), retry_pw_.c_str());
    } else {
      state_.store(State::kIdle);
    }
  };

  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  while (xTaskGetTickCount() < deadline) {
    if (state_.load() == State::kConnected) {
      // Creds work. Leave the candidate associated — the caller saves the
      // new creds and reboots, so resuming the old target would be wasted.
      return ESP_OK;
    }
    const uint8_t reason = last_reason_.load();
    const bool terminal = reason == 201 || reason == 202 || reason == 203 ||
                          reason == 204 || reason == 205;
    if (terminal) {
      resume_saved();
      return ESP_ERR_INVALID_RESPONSE;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // No conclusive result within the deadline.
  resume_saved();
  return ESP_ERR_TIMEOUT;
}

esp_err_t Wifi::WaitConnected(uint32_t timeout_ms) {
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  while (state_.load() != State::kConnected) {
    if (xTaskGetTickCount() >= deadline) return ESP_ERR_TIMEOUT;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return ESP_OK;
}

std::string Wifi::ip() const {
  return Ipv4ToString(ip_.load());
}

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
  // APSTA needs BOTH netifs, otherwise STA can associate but DHCP
  // never runs and no GOT_IP event fires — breaks the provisioning
  // portal's TryConnect verify-before-save. Create the STA netif
  // alongside the AP one even though the current code path doesn't
  // call Start() on the empty-SSID branch.
  if (!netif_sta_) netif_sta_ = esp_netif_create_default_wifi_sta();
  if (!netif_sta_) return ESP_FAIL;

  if (!started_) {
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), kTag, "wifi_init.ap");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, EventTrampoline, this,
                            &wifi_event_instance_),
                        kTag, "reg wifi evt ap");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, ESP_EVENT_ANY_ID, EventTrampoline, this,
                            &ip_event_instance_),
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
    ap_cfg.ap.pmf_cfg.required = false;  // compatibility with WPA2-only clients
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

esp_err_t Wifi::StopSoftAp() {
  if (!ap_mode_.load()) return ESP_OK;
  // APSTA -> STA drops the AP beacon but keeps the STA association. The
  // background auto-retry (sta_auto_retry_) is untouched, so a STA that
  // wasn't yet connected keeps trying.
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "set STA");
  ap_mode_.store(false);
  ESP_LOGI(kTag, "SoftAP down, STA-only");
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
    for (auto& u : uniq)
      if (u.ssid == e.ssid) {
        seen = true;
        break;
      }
    if (!seen) uniq.push_back(std::move(e));
  }
  return uniq;
}

esp_err_t Wifi::StartBackgroundScan(uint32_t refresh_ms) {
  if (!started_) {
    ESP_LOGW(kTag, "StartBackgroundScan: wifi not started");
    return ESP_ERR_INVALID_STATE;
  }

  if (!scan_timer_) {
    esp_timer_create_args_t targs = {};
    targs.callback = &Wifi::ScanTimerTrampoline;
    targs.arg = this;
    targs.dispatch_method = ESP_TIMER_TASK;
    targs.name = "wifi_scan";
    esp_err_t err = esp_timer_create(&targs, &scan_timer_);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "scan timer create: %s", esp_err_to_name(err));
      return err;
    }
  }

  // Stop any prior periodic schedule, then restart. Idempotent.
  esp_timer_stop(scan_timer_);
  esp_err_t err = esp_timer_start_periodic(
      scan_timer_, static_cast<uint64_t>(refresh_ms) * 1000ULL);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "scan timer start: %s", esp_err_to_name(err));
    return err;
  }

  // Kick off the first scan immediately so the cache warms up without
  // waiting a full refresh interval.
  {
    std::lock_guard<std::mutex> lock(scan_mu_);
    TriggerScanLocked();
  }
  ESP_LOGI(kTag, "background scan started (refresh=%u ms)",
           static_cast<unsigned>(refresh_ms));
  return ESP_OK;
}

void Wifi::StopBackgroundScan() {
  if (scan_timer_) {
    esp_timer_stop(scan_timer_);
  }
}

std::vector<WifiScanEntry> Wifi::GetCachedScan() const {
  std::lock_guard<std::mutex> lock(scan_mu_);
  return scan_cache_;  // copy out under lock
}

esp_err_t Wifi::TriggerScanLocked() {
  // Caller holds scan_mu_. We only guard `scan_in_flight_` with an atomic
  // so the event handler can clear it without taking the mutex.
  if (scan_in_flight_.load()) return ESP_OK;  // already scanning

  wifi_scan_config_t cfg = {};
  cfg.show_hidden = false;
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  cfg.scan_time.active.min = 120;
  cfg.scan_time.active.max = 180;

  scan_in_flight_.store(true);
  esp_err_t err = esp_wifi_scan_start(&cfg, /*block=*/false);
  if (err != ESP_OK) {
    scan_in_flight_.store(false);
    ESP_LOGW(kTag, "async scan_start err=%s", esp_err_to_name(err));
  }
  return err;
}

void Wifi::ScanTimerTrampoline(void* arg) {
  auto* self = static_cast<Wifi*>(arg);
  std::lock_guard<std::mutex> lock(self->scan_mu_);
  self->TriggerScanLocked();
}

void Wifi::OnScanDone() {
  // Only consume results if we kicked off the scan asynchronously; the
  // blocking Scan() path retrieves its own results.
  if (!scan_in_flight_.exchange(false)) return;

  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count > 32) ap_count = 32;

  std::vector<WifiScanEntry> out;
  if (ap_count > 0) {
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
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.rssi > b.rssi; });
    std::vector<WifiScanEntry> uniq;
    uniq.reserve(out.size());
    for (auto& e : out) {
      bool seen = false;
      for (auto& u : uniq)
        if (u.ssid == e.ssid) {
          seen = true;
          break;
        }
      if (!seen) uniq.push_back(std::move(e));
    }
    out = std::move(uniq);
  }

  {
    std::lock_guard<std::mutex> lock(scan_mu_);
    scan_cache_ = std::move(out);
  }
  scan_ready_.store(true);
  ESP_LOGI(kTag, "background scan done, cached %u networks",
           static_cast<unsigned>(ap_count));
}

void Wifi::EventTrampoline(void* arg, esp_event_base_t base, int32_t id,
                           void* data) {
  static_cast<Wifi*>(arg)->OnEvent(base, id, data);
}

void Wifi::OnEvent(esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
    OnScanDone();
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
    const uint8_t reason = ev ? ev->reason : 0;
    ESP_LOGW(kTag, "disconnected reason=%u", reason);
    last_reason_.store(reason);
    // Terminal reasons flag creds/AP faults that won't self-heal; non-
    // terminal reasons (AP reboot, roaming, interference) are transient
    // and MUST NOT bump the N-strikes counter or we'd nuke good creds
    // every time a router hiccups.
    const bool terminal = reason == 201 ||  // NO_AP_FOUND
                          reason == 202 ||  // AUTH_FAIL
                          reason == 203 ||  // ASSOC_FAIL
                          reason == 204 ||  // HANDSHAKE_TIMEOUT
                          reason == 205;    // CONNECTION_FAIL
    if (terminal) {
      terminal_strikes_.fetch_add(1);
    }
    state_.store(State::kDisconnected);
    ip_.store(0);
    // Auto-retry whenever a STA association is wanted. Gated on the
    // explicit sta_auto_retry_ flag (NOT ap_mode_): in APSTA fallback the
    // SoftAP is up yet we still want to keep retrying the saved network;
    // during a portal verify TryConnect pauses this so the two attempts
    // don't race on the shared radio.
    if (sta_auto_retry_.load()) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      state_.store(State::kConnecting);
      esp_wifi_connect();
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* ev = static_cast<ip_event_got_ip_t*>(data);
    ip_.store(ev->ip_info.ip.addr);
    state_.store(State::kConnected);
    // Successful association clears the failure counter — one good
    // connect wipes out all prior strikes, matching the "transient AP
    // reboot" recovery path.
    terminal_strikes_.store(0);
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
