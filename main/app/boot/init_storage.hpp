// NVS + TZ + LittleFS boot-time initialisation.
//
// Runs after InitPanelsAndSplash (so a corrupted NVS doesn't brick the
// splash) but before any subsystem that reads a persisted setting:
//   * Prefs::InitOnce() — open the default NVS partition.
//   * epd::SetGlobalInverted() — restore the panel polarity flag.
//   * timezone::InitFromNvs() — setenv("TZ", ...) + tzset() for the
//     clock screen + logging timestamps.
//   * MountLittleFs() — format-on-failure so a fresh flash self-heals;
//     mount failures are logged but non-fatal.
//   * (optional) RunLittleFsSelfTest() when CONFIG_BTCLOCK_LITTLEFS_SELFTEST
//     is set.
//
// Owns no AppCtx state — everything here writes to global process-wide
// state (setenv, NVS handle, EPD polarity). Takes AppCtx by reference
// for consistency with the rest of the init_* TUs.

#pragma once

namespace btclock {

struct AppCtx;

void InitStorage(AppCtx& ctx);

}  // namespace btclock
