#include "app/boot/init_network.hpp"

#include <cstdint>
#include <memory>
#include <string>

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "app/boot/init_mdns.hpp"
#include "app/network_coordinator.hpp"
#include "board/board.hpp"
#include "boot_spinner.hpp"
#include "control_server.hpp"
#include "data_core/hub.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io/led_controller.hpp"
#include "io/network_led_watchdog.hpp"
#include "io/wifi_guard.hpp"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "prefs.hpp"
#include "provisioning_server.hpp"
#include "provisioning_ui.hpp"
#include "sdkconfig.h"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock";
}  // namespace

void StartProvisioningPortal(AppCtx& ctx, bool render) {
  // The control server (have-creds boot) is already bound to port 80, so
  // free it before the portal's httpd tries the same port — otherwise
  // httpd_start fails with EADDRINUSE. No-op in pure-provisioning boot,
  // where InitControlApi skipped the control server (ctx.ctrl is null).
  if (ctx.ctrl) ctx.ctrl->StopHttpd();
  // Run AP+STA so the saved-network STA retry (when present) keeps running
  // alongside the portal. StartSoftAp sets WIFI_MODE_APSTA.
  ESP_ERROR_CHECK(
      ctx.wifi->StartSoftAp(ctx.ap_ssid.c_str(), ctx.ap_pw.c_str()));
  // Warm the SSID scan cache in the background so /api/scan returns
  // instantly once the portal page loads.
  ctx.wifi->StartBackgroundScan();

  ctx.portal = std::make_unique<ProvisioningServer>(
      *ctx.wifi, board::kHardwareName,
      [](const std::string& new_ssid, const std::string& new_pw) {
        Prefs np("net");
        np.SetString("ssid", new_ssid.c_str());
        np.SetString("pw", new_pw.c_str());
        np.Commit();
        // wifiConfigured: arms the wpTimeout reboot watchdog on future
        // portal sessions so a stale device that drops back into AP mode
        // (AP changed password, moved, etc.) retries STA after the timeout
        // instead of sitting in the portal forever.
        Prefs settings(prefs::kSettingsNs);
        settings.SetBool(prefs::kWifiConfigured, true);
        settings.Commit();
        ESP_LOGI("btclock", "creds saved; rebooting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
      });
  ESP_ERROR_CHECK(ctx.portal->Start());

  ip4_addr_t ap_ip4 = {};
  inet_pton(AF_INET, ctx.wifi->ap_ip().c_str(), &ap_ip4);
  ctx.dns = std::make_unique<DnsHijack>(ap_ip4.addr);
  ESP_ERROR_CHECK(ctx.dns->Start());

  // The empty-creds boot path passes render=false: fonts aren't loaded
  // until InitScreenManager, so DispatchBootPath paints the portal UI
  // later in the boot sequence. The fallback coordinator runs in the
  // event loop (fonts long since loaded) and passes render=true.
  if (render) {
    StopBootSpinner();
    RenderProvisioningScreen(ctx.panels, AppCtx::fb_storage(), ctx.fonts,
                             ctx.ap_ssid, ctx.ap_pw);
    PostLedEffect(LedEffect::kSetProvisioning);
  }
}

void StopProvisioningPortal(AppCtx& ctx) {
  // Destructors stop the DNS task + httpd; then drop the AP beacon.
  ctx.dns.reset();
  ctx.portal.reset();
  ctx.wifi->StopBackgroundScan();
  ctx.wifi->StopSoftAp();
  // Port 80 is free again (portal httpd destructed above) — resume the
  // control server we paused when the fallback came up.
  if (ctx.ctrl && ctx.ctrl->Start() != ESP_OK) {
    ESP_LOGW(kTag, "control server resume failed after portal teardown");
  }
  // Repaint the normal screen to clear the portal UI. The hub exists by
  // now (data sources were wired at boot), so this paints real data when
  // available, placeholders otherwise.
  if (ctx.sm) {
    ctx.sm->Render(ctx.panels, AppCtx::fb_storage(), ctx.fonts,
                   ctx.hub ? ctx.hub->GetSnapshot() : DataSnapshot{});
  }
}

void InitNetwork(AppCtx& ctx) {
  Prefs net_prefs("net");
  const std::string ssid =
      net_prefs.GetString("ssid", CONFIG_BTCLOCK_WIFI_SSID);
  const std::string pw =
      net_prefs.GetString("pw", CONFIG_BTCLOCK_WIFI_PASSWORD);

  ctx.wifi = std::make_unique<Wifi>();
  ctx.sta_ssid = ssid;
  // AP creds are prepared unconditionally: the empty-creds path enters the
  // portal immediately, and the have-creds path may bring the portal up as
  // a fallback if STA can't reach the saved network within the grace
  // window (NetworkCoordinator).
  ctx.ap_ssid = MakeApSsid();
  ctx.ap_pw = MakeOrLoadApPassword(net_prefs);

  // Soft watchdog for multi-minute STA outages. Loaded here so the
  // setting applies even before the main loop starts pumping ticks.
  // Default 10 min matches Arduino main.cpp::checkWiFiConnection();
  // range clamp mirrors the schema (0 disables, ≤ 120).
  uint32_t wifi_reboot_minutes = 10;
  {
    Prefs settings_for_wifi(prefs::kSettingsNs);
    wifi_reboot_minutes =
        btclock::settings::ReadU32(settings_for_wifi, prefs::kWifiRebootMin);
    if (wifi_reboot_minutes > 120) wifi_reboot_minutes = 120;
  }
  ctx.outage_watchdog = std::make_unique<OutageWatchdog>(wifi_reboot_minutes);
  // LED-only network watchdog: WiFi up/down + per-source stale signal.
  // Probes are wired later by init_control_api.cpp once the data sources
  // exist; here we just construct the empty state machine and bind the
  // wifi pointer.
  ctx.network_led_watchdog = std::make_unique<NetworkLedWatchdog>();
  ctx.network_led_watchdog->SetWifi(ctx.wifi.get());

  if (!ssid.empty()) {
    ESP_ERROR_CHECK(ctx.wifi->Start());
    // txPower: persisted via control_server's HandleWifiTxPower, but the
    // setting wasn't re-applied at boot before — a saved value lasted
    // only until the next reboot. Apply between esp_wifi_start (Wifi::
    // Start) and Connect so the radio honours the limit on the first
    // association. Validation mirrors the live PATCH path
    // (control_server.cpp HandleWifiTxPower + settings_api.cpp's txPower
    // branch): -1..78 sets the cap, 80 means "no override", anything else
    // (incl. NVS-missing) is left at the IDF default. INT32_MIN is a
    // sentinel that survives the u32 round-trip in NvsPrefs::GetI32 — it
    // lets us distinguish "key absent" from a stored 0.
    {
      settings::NvsPrefs settings_for_tx(prefs::kSettingsNs);
      const int32_t tx = settings_for_tx.GetI32(prefs::kTxPower, INT32_MIN);
      if (tx >= -1 && tx <= 78) {
        esp_wifi_set_max_tx_power(static_cast<int8_t>(tx));
      }
    }
    // Push the user's `<hostnamePrefix>-<mac6>` into the STA netif's DHCP
    // option-12 hostname BEFORE Connect — DHCP DISCOVER fires on
    // association, so a later set wouldn't surface in the router's client
    // list until the next lease renewal.
    ApplyDhcpHostname();
    // Non-blocking: kick off the association and let the boot sequence
    // proceed. NetworkCoordinator (ticked by the event loop) wires the
    // internet services on the first GOT_IP and — if the saved network
    // can't be reached within the grace window — brings up the
    // provisioning portal CONCURRENTLY while STA keeps retrying. Creds are
    // never wiped; a recovered network just reconnects on its own.
    ESP_ERROR_CHECK(ctx.wifi->Connect(ssid.c_str(), pw.c_str()));
    PostLedEffect(LedEffect::kWifiConnecting);
    ctx.net_coordinator = std::make_unique<NetworkCoordinator>(ctx);
  } else {
    ESP_LOGW(kTag, "provisioning mode: SoftAP '%s' pw='%s'",
             ctx.ap_ssid.c_str(), ctx.ap_pw.c_str());
    // render=false: DispatchBootPath paints the portal UI after fonts load.
    StartProvisioningPortal(ctx, /*render=*/false);

    // wpTimeout: abandon the portal after N seconds and reboot so a new
    // STA attempt runs on the next boot. Mirrors v3's WiFiManager
    // setConfigPortalTimeout. Gated on a previously configured SSID — if
    // the user has never submitted creds, sitting in the portal is the
    // only thing the device CAN do, so reboot would just spin. 0 disables.
    // Range clamp matches the schema (0..3600). The Save callback already
    // reboots on its own success path, so this only fires when the user
    // never submits.
    {
      Prefs settings_for_wp(prefs::kSettingsNs);
      uint32_t wp_timeout_s =
          btclock::settings::ReadU32(settings_for_wp, prefs::kWpTimeout);
      if (wp_timeout_s > 3600) wp_timeout_s = 3600;
      const bool prior_creds =
          settings_for_wp.GetBool(prefs::kWifiConfigured, false);
      if (wp_timeout_s > 0 && prior_creds) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) {
          ESP_LOGW("btclock", "wpTimeout elapsed; rebooting to retry STA");
          esp_restart();
        };
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "wpTimeout";
        esp_timer_handle_t h = nullptr;
        if (esp_timer_create(&args, &h) == ESP_OK) {
          esp_timer_start_once(
              h, static_cast<uint64_t>(wp_timeout_s) * 1000ULL * 1000ULL);
        }
      }
    }
  }
}

}  // namespace btclock
