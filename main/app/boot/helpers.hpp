// Small boot-time helpers used by multiple init TUs.
//
// Kept here (instead of inside a specific init_*.cpp) because the
// millisecond-timestamp helper is consumed by every screen render call
// and by the TimerAdapter, and the AP-SSID / AP-password helpers are
// shared between InitNetwork and the factory-reset splash (which
// always happens in STA mode but the factory-reset path reloads
// them on reboot).

#pragma once

#include <cstdint>
#include <string>

#include "prefs.hpp"
#include "sdkconfig.h"

namespace btclock {

// esp_timer_get_time() returns microseconds since boot; divide by 1000
// for the ms granularity every ScreenManager / DataHub consumer uses.
int64_t MsNow();

// "BTClock-XXXX" where XXXX is the last two MAC bytes (SoftAP MAC,
// which differs from the STA MAC by one bit on ESP32 — so AP and STA
// show up as distinct on Wi-Fi scans). Pure-logic formatter lives in
// main/net_util.hpp; this wraps the ESP-IDF MAC read.
std::string MakeApSsid();

// Load the persisted SoftAP password from NVS namespace "net" or,
// on first boot, generate a random one and persist it. 8-character
// password from an ambiguity-free character set (see net_util.hpp).
std::string MakeOrLoadApPassword(btclock::Prefs& prefs);

#ifdef CONFIG_BTCLOCK_LITTLEFS_SELFTEST
// Round-trip a known blob on the mounted LittleFS partition and log
// the outcome. Swallows errors so a degraded partition never bricks
// boot. Guarded by CONFIG_BTCLOCK_LITTLEFS_SELFTEST (Kconfig, off by
// default).
void RunLittleFsSelfTest(const char* base_path);
#endif

}  // namespace btclock
