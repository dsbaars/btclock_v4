// Screen-kind enum, split out of screens.hpp so pure-logic helpers
// (panel_texts, host tests) can include it without pulling in the EPD
// driver or font headers screens.hpp drags in.

#pragma once

#include <cstdint>

namespace btclock {

// Single source of truth for the screen catalogue. Each row is
//   X(enum_name, api_id, short_name, display_label)
// where:
//   - `enum_name`     maps to ScreenType::<name>, used for C++ dispatch.
//   - `api_id`        is the stable id the WebUI persists in `screens[].id`
//                     and in the `screenOrder` / `screen<id>Visible` NVS
//                     keys. Must match the old Arduino firmware's
//                     `ScreenMapping`; WebUI stringify-compares these so
//                     renumbering would drop user rotation preferences on
//                     upgrade.
//   - `short_name`    is the KindName() tag used in logs / SSE events.
//   - `display_label` is the human name surfaced in GET /api/settings
//                     `screens[].name`. Matches the old firmware's table
//                     so existing WebUI translations (m['screens.<label>'])
//                     keep resolving.
// kNostrZap is deliberately excluded from this list — same rationale
// as kCustom / kDebug below: it's a push-driven override, not a user-
// rotatable catalogue entry. api_id 70 is owned by kMiningPoolHashrate
// (matches the old firmware's SCREEN_MINING_POOL_STATS_HASHRATE).
#define BTCLOCK_SCREEN_KIND_LIST(X)                                     \
  X(kBlockHeight,         0,  "block",      "Block Height")             \
  X(kClock,               3,  "clock",      "Time")                     \
  X(kHalving,             4,  "halving",    "Halving countdown")        \
  X(kBlockFeeRate,        6,  "fee",        "Block Fee Rate")           \
  X(kMoscowTime,          10, "moscow",     "Sats per dollar")          \
  X(kBtcPrice,            20, "price",      "Ticker")                   \
  X(kMarketCap,           30, "mcap",       "Market Cap")               \
  X(kBitcoinSupply,       40, "supply",     "Bitcoin Supply")           \
  X(kMiningPoolHashrate,  70, "poolhash",   "Mining Pool Hashrate")     \
  X(kMiningPoolEarnings,  71, "poolearn",   "Mining Pool Earnings")     \
  X(kBitaxeHashrate,      80, "bxhash",     "Bitaxe Hashrate")          \
  X(kBitaxeBestDiff,      81, "bxdiff",     "Bitaxe Best Difficulty")

// Which top-level screen is currently being displayed. Screen rotation
// and button navigation cycle through these. Values are dense 0..N-1
// for switch dispatch; the WebUI-visible "id" lives in the X-macro
// above (which intentionally excludes kCustom and kDebug since they
// are not in the rotation catalogue).
//
// `kCustom` is the runtime-pushed override driven by POST /api/show/text
// and /api/show/custom. It is never in the auto-rotation list — the
// client explicitly steps onto it and any subsequent nav (button press,
// auto-rotate tick) exits back to the normal cycle. Mirrors the old
// firmware's SCREEN_CUSTOM (src/lib/system/shared.hpp:67).
//
// `kDebug` is the off-rotation diagnostic overlay triggered by the
// button-4 press — a second press pops back to the data screen that
// was up before entry.
//
// `kNostrZap` is a transient notification override triggered by an
// incoming NIP-57 zap receipt. Like kCustom it is push-driven and
// never appears in the rotation catalogue; unlike kCustom it auto-
// exits after a short timeout (see ScreenManager::SetZapNotify).
enum class ScreenType : uint8_t {
  kBlockHeight,
  kMoscowTime,
  kBtcPrice,
  kBlockFeeRate,
  kClock,
  kHalving,
  kBitcoinSupply,
  kMarketCap,
  kMiningPoolHashrate,
  kMiningPoolEarnings,
  kBitaxeHashrate,
  kBitaxeBestDiff,
  kCustom,
  kDebug,
  kNostrZap,
  // Painted once by the OTA push-upload path via RenderOtaUpdateScreen.
  // Outranks every other override while OtaManager's push flow is
  // active; the rotation timer is frozen and ShouldRender() returns
  // false so a data-push can't stomp the overlay mid-write. The HTTP
  // worker task is the one painting the panels here — the main render
  // loop short-circuits while this is active.
  kOtaUpdate,
};

}  // namespace btclock
