// WiFi guard — block the caller until the STA gets an IP, so network-
// dependent subsystems (data sources, NTP, etc.) come up cleanly.
//
// No timeout: on bad credentials esp_wifi retries indefinitely and this
// function just keeps waiting. The splash screen stays visible throughout,
// and the user can reprovision via the captive portal if NVS is wrong
// (only entered on an empty SSID, so credential edit needs a power cycle).

#pragma once

#include <cstdint>

namespace btclock {

class Wifi;

// Blocks until wifi.state() == kConnected. Logs a "still no IP" line
// every `log_every_ms` milliseconds so the serial console shows progress
// during slow association (DHCP, auth retries).
void WaitForConnected(Wifi& wifi, uint32_t log_every_ms = 10'000);

}  // namespace btclock
