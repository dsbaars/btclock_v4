// mDNS advertisement bring-up. Called from app_main after the network
// stack is up. A no-op when:
//   - Wi-Fi is in SoftAP (captive-portal) mode — the provisioning flow
//     owns DNS hijacking and mDNS would fight it for port 5353.
//   - The `mdnsEnabled` pref is false (default true — parity with the
//     old Arduino firmware's DEFAULT_MDNS_ENABLED).
//
// The advertised hostname and TXT records mirror btclock_v3_fci so
// existing discovery clients (macOS Bonjour, avahi-browse, WebUI scan
// helpers) see the same shape after the IDF port.

#pragma once

namespace btclock {

struct AppCtx;

// Start mDNS. Logs-and-continues on any failure so a misbehaving mDNS
// stack (e.g. no IPv4 link yet) never blocks boot. Safe to call at
// most once per boot; subsequent calls are ignored.
void InitMdns(AppCtx& ctx);

}  // namespace btclock
