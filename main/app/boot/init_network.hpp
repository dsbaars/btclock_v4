// Wi-Fi (STA or SoftAP), provisioning portal, DNS hijack, SNTP sync,
// and the long-outage soft watchdog.
//
// Reads the STA credentials from NVS namespace "net" (keys "ssid",
// "pw") with a fallback to CONFIG_BTCLOCK_WIFI_* sdkconfig values. Empty
// SSID puts the device in SoftAP provisioning mode: a generated
// BTClock-XXXX SSID + persistent random password, a captive
// ProvisioningServer, and a DNS hijack that sends every query to the
// AP gateway. A non-empty SSID kicks off a normal STA connect +
// SNTP sync.
//
// OutageWatchdog is loaded in both branches so the main event loop's
// Tick() call is valid regardless of boot mode (AP mode skips the
// actual pumping — the guard lives in the event loop).
//
// AppCtx fields populated: wifi, portal, dns, ap_ssid, ap_pw,
// sta_ssid, outage_watchdog.

#pragma once

namespace btclock {

struct AppCtx;

void InitNetwork(AppCtx& ctx);

}  // namespace btclock
