#pragma once

#include <cstdint>

// General-purpose on-device diagnostics: mirror esp_log over UDP.
//
// The console is on USB-Serial-JTAG, which is unreadable from the dev
// host in this environment (passive reads yield zero bytes even across a
// reset), so route every esp_log line over the LAN instead. Installs an
// esp_log_set_vprintf hook that UDP-broadcasts each formatted log line on
// `port` while keeping the original serial vprintf intact. Build is
// WARN-max, so only ESP_LOGW and above are emitted. Capture on the host
// with a UDP socket bound to that port (e.g. `nc -ul 9999`).
//
// Compiled to a no-op stub unless BTCLOCK_DIAG_UDP_LOG is defined (see
// root CMakeLists; BTCLOCK_DIAG_NWC_FLASH implies it). Call once, after
// the network (esp_netif + lwip) is up.
namespace btclock {
void InstallUdpLogSink(uint16_t port = 9999);
}  // namespace btclock
