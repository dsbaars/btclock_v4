// mDNS advertisement bring-up. Called from app_main after the network
// stack is up. A no-op when:
//   - Wi-Fi is in SoftAP (captive-portal) mode — the provisioning flow
//     owns DNS hijacking and mDNS would fight it for port 5353.
//   - The `mdnsEnabled` pref is false (default true — parity with the
//     old Arduino firmware's DEFAULT_MDNS_ENABLED).
//
// The advertised hostname and TXT records mirror the v3 firmware so
// existing discovery clients (macOS Bonjour, avahi-browse, WebUI scan
// helpers) see the same shape after the IDF port.

#pragma once

namespace btclock {

struct AppCtx;

// Start mDNS. Logs-and-continues on any failure so a misbehaving mDNS
// stack (e.g. no IPv4 link yet) never blocks boot. Safe to call at
// most once per boot; subsequent calls are ignored — use ReinitMdns to
// re-publish after a settings change.
void InitMdns(AppCtx& ctx);

// Re-publish the mDNS advert with whatever `mdnsEnabled` /
// `hostnamePrefix` currently say in NVS. Wired from
// `ControlServer::on_mdns_changed` so PATCH /api/settings takes effect
// without reboot. Tears down the previous responder (if any) before
// re-running the boot-time setup, so it's safe to call repeatedly even
// when the user toggles `mdnsEnabled` off (frees the responder) or back
// on (re-publishes). Runs on the httpd worker — keep the mdns lib calls
// inside the file private to this TU.
void ReinitMdns();

// Push the user's `<hostnamePrefix>-<mac6>` string into the STA netif's
// DHCP option-12 hostname field. Without this the device announces as
// CONFIG_LWIP_LOCAL_HOSTNAME (the IDF default is "espressif"), which is
// what every BTClock owner saw in their router's client list.
//
// Call sites:
//   - InitNetwork, after esp_wifi_start and BEFORE wifi->Connect — the
//     DHCP DISCOVER goes out on Connect, so the hostname must be in
//     place by then.
//   - ReinitMdns, when a hostnamePrefix PATCH lands. esp_netif_set_hostname
//     updates the cached value but does not force a DHCP RENEW; the
//     router picks up the new name at the next lease cycle. Same caveat
//     already documented for the mDNS re-publish path.
//
// Lives next to init_mdns because both consumers compute the same
// hostname via net_util::ComputeHostname and need to stay in lockstep.
// Returns ESP_OK on success or an esp_err_t from the netif call. Logs
// and continues on failure — a stale name is preferable to refusing to
// associate.
void ApplyDhcpHostname();

}  // namespace btclock
