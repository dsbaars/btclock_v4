#include "app/boot/init_network.hpp"

#include <memory>
#include <string>

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "app/time_sync.hpp"
#include "io/wifi_guard.hpp"
#include "board/board.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "prefs.hpp"
#include "provisioning_server.hpp"
#include "sdkconfig.h"
#include "settings/pref_keys.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock";
}  // namespace

void InitNetwork(AppCtx& ctx) {
  Prefs net_prefs("net");
  const std::string ssid =
      net_prefs.GetString("ssid", CONFIG_BTCLOCK_WIFI_SSID);
  const std::string pw =
      net_prefs.GetString("pw", CONFIG_BTCLOCK_WIFI_PASSWORD);

  ctx.wifi = std::make_unique<Wifi>();
  ctx.sta_ssid = ssid;

  // Soft watchdog for multi-minute STA outages. Loaded here so the
  // setting applies even before the main loop starts pumping ticks.
  // Default 10 min matches Arduino main.cpp::checkWiFiConnection();
  // range clamp mirrors the schema (0 disables, ≤ 120).
  uint32_t wifi_reboot_minutes = 10;
  {
    Prefs settings_for_wifi(prefs::kSettingsNs);
    wifi_reboot_minutes =
        settings_for_wifi.GetU32(prefs::kWifiRebootMin, 10);
    if (wifi_reboot_minutes > 120) wifi_reboot_minutes = 120;
  }
  ctx.outage_watchdog =
      std::make_unique<OutageWatchdog>(wifi_reboot_minutes);

  if (!ssid.empty()) {
    ESP_ERROR_CHECK(ctx.wifi->Start());
    ESP_ERROR_CHECK(ctx.wifi->Connect(ssid.c_str(), pw.c_str()));
    WaitForConnected(*ctx.wifi, net_prefs);
    ESP_ERROR_CHECK(StartSntpSync());
  } else {
    ctx.ap_ssid = MakeApSsid();
    ctx.ap_pw = MakeOrLoadApPassword(net_prefs);
    ESP_LOGW(kTag, "provisioning mode: SoftAP '%s' pw='%s'",
             ctx.ap_ssid.c_str(), ctx.ap_pw.c_str());
    ESP_ERROR_CHECK(
        ctx.wifi->StartSoftAp(ctx.ap_ssid.c_str(), ctx.ap_pw.c_str()));
    // Warm the SSID scan cache in the background so /api/scan can return
    // instantly once the portal page loads.
    ctx.wifi->StartBackgroundScan();

    ctx.portal = std::make_unique<ProvisioningServer>(
        *ctx.wifi, board::kHardwareName,
        [](const std::string& new_ssid, const std::string& new_pw) {
          Prefs np("net");
          np.SetString("ssid", new_ssid.c_str());
          np.SetString("pw", new_pw.c_str());
          np.Commit();
          ESP_LOGI("btclock", "creds saved; rebooting");
          vTaskDelay(pdMS_TO_TICKS(1000));
          esp_restart();
        });
    ESP_ERROR_CHECK(ctx.portal->Start());

    ip4_addr_t ap_ip4 = {};
    inet_pton(AF_INET, ctx.wifi->ap_ip().c_str(), &ap_ip4);
    ctx.dns = std::make_unique<DnsHijack>(ap_ip4.addr);
    ESP_ERROR_CHECK(ctx.dns->Start());
  }
}

}  // namespace btclock
