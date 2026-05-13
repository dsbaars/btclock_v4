// Pure-logic helpers that translate between ScreenManager's dense
// rotation slots and the settings-catalog `screens[].id` (api_id) the
// WebUI uses in its picker and the persisted `screens` array.
//
// Slot layout (must stay in lock-step with ScreenManager::current_kind):
//
//   slot 0          : kBlockHeight          api_id 0
//   slot 1          : kClock                api_id 3
//   slot 2          : kHalving              api_id 4
//   slot 3          : kBitcoinSupply        api_id 40
//   slot 4          : kMiningPoolHashrate   api_id 70
//   slot 5          : kMiningPoolEarnings   api_id 71
//   slot 6          : kBitaxeHashrate       api_id 80
//   slot 7          : kBitaxeBestDiff       api_id 81
//   slot 8          : kNwcBalance           api_id 90
//   slot 9 + 3k     : kMoscowTime           api_id 10   for currencies[k]
//   slot 10 + 3k    : kBtcPrice             api_id 20   for currencies[k]
//   slot 11 + 3k    : kMarketCap            api_id 30   for currencies[k]
//   slot last       : kBlockFeeRate         api_id 6
//
// The api_id ↔ slot relationship is many-to-one for per-currency screens
// (one api_id, N slots — one per active currency). The forward
// (api_id → slot) helpers land on a caller-chosen preferred-currency
// index; the inverse (slot → api_id) is total.
//
// Kept in a header so both the ScreenManager (device) and host tests
// (no ESP-IDF) can link the same implementation without a .cpp pulling
// in FreeRTOS / EPD bits.

#pragma once

#include <cstddef>

namespace btclock {
namespace slot_map {

// Catalogue ids that match BTCLOCK_SCREEN_KIND_LIST in screens/screen_kind.hpp
// and main/app/catalogs.hpp. Stable across firmware versions — renumbering
// would break the persisted `screenOrder` and `screen<id>Visible` NVS keys.
inline constexpr int kApiIdBlockHeight = 0;
inline constexpr int kApiIdClock = 3;
inline constexpr int kApiIdHalving = 4;
inline constexpr int kApiIdBlockFeeRate = 6;
inline constexpr int kApiIdMoscowTime = 10;
inline constexpr int kApiIdBtcPrice = 20;
inline constexpr int kApiIdMarketCap = 30;
inline constexpr int kApiIdBitcoinSupply = 40;
inline constexpr int kApiIdMiningPoolHashrate = 70;
inline constexpr int kApiIdMiningPoolEarnings = 71;
inline constexpr int kApiIdBitaxeHashrate = 80;
inline constexpr int kApiIdBitaxeBestDiff = 81;
inline constexpr int kApiIdNwcBalance = 90;

// Default value for `screen<id>Visible` when the NVS key is absent.
// Most screens default ON so a fresh device shows the full rotation;
// kApiIdNwcBalance defaults OFF (privacy — some operators don't want
// their NWC balance glanceable to bystanders). The user opts in via
// the WebUI screen picker; `screen90Visible=true` then sticks per
// device. Mining-pool / bitaxe ids still default ON here because the
// parent-feature gate (miningPoolStats / bitaxeEnabled) already
// suppresses them when the feature isn't configured.
inline constexpr bool DefaultScreenVisible(int api_id) {
  return api_id != kApiIdNwcBalance;
}

// Currency-agnostic slots before per-currency fan-out:
//   0=block, 1=clock, 2=halving, 3=supply,
//   4=mining-pool-hashrate, 5=mining-pool-earnings,
//   6=bitaxe-hashrate, 7=bitaxe-best-diff,
//   8=nwc-balance.
// Mining-pool + bitaxe + NWC slots stay in the rotation whether or
// not the user has the feature enabled; the renderers paint an
// OFFLINE / placeholder frame when no data has arrived so slot_count
// stays stable across pref flips — persisted screenOrder keeps
// working.
inline constexpr std::size_t kAgnosticSlots = 9;
inline constexpr std::size_t kPerCurrencySlots = 3;

inline std::size_t SlotCount(std::size_t currency_count) {
  return kAgnosticSlots + kPerCurrencySlots * currency_count + 1;
}

// slot → api_id. Total: every slot in [0, SlotCount(currency_count))
// maps to exactly one api_id. `slot >= SlotCount` clamps to -1 so
// callers can detect drift (stale mirror, race during reconfig).
inline int ApiIdForSlot(std::size_t slot, std::size_t currency_count) {
  const std::size_t total = SlotCount(currency_count);
  if (slot >= total) return -1;
  if (slot == total - 1) return kApiIdBlockFeeRate;
  switch (slot) {
    case 0:
      return kApiIdBlockHeight;
    case 1:
      return kApiIdClock;
    case 2:
      return kApiIdHalving;
    case 3:
      return kApiIdBitcoinSupply;
    case 4:
      return kApiIdMiningPoolHashrate;
    case 5:
      return kApiIdMiningPoolEarnings;
    case 6:
      return kApiIdBitaxeHashrate;
    case 7:
      return kApiIdBitaxeBestDiff;
    case 8:
      return kApiIdNwcBalance;
    default:
      break;
  }
  const std::size_t off = (slot - kAgnosticSlots) % kPerCurrencySlots;
  switch (off) {
    case 0:
      return kApiIdMoscowTime;
    case 1:
      return kApiIdBtcPrice;
    case 2:
      return kApiIdMarketCap;
  }
  return -1;
}

// Translate `current_slot` to the same per-currency kind under a
// different currency, so POST /api/show/currency preserves the user's
// position within the Moscow/Price/MarketCap triple (e.g. Market Cap
// USD → Market Cap EUR, not Market Cap USD → Moscow Time EUR).
//
// For agnostic slots (block height, clock, halving, supply) or the
// fee-rate slot, there is no per-currency offset to carry; the caller
// gets the Moscow slot of the new currency — matching the old
// setCurrentCurrency behaviour where switching currency also reset
// the rotation to the first per-currency screen.
//
// `new_currency_index` is trusted to be in `[0, currency_count)` —
// callers resolve the currency name before calling.
inline std::size_t TransposeSlotToCurrency(std::size_t current_slot,
                                           std::size_t new_currency_index,
                                           std::size_t currency_count) {
  const std::size_t total = SlotCount(currency_count);
  const bool is_fee_rate_slot =
      (currency_count > 0 && current_slot == total - 1);
  std::size_t off = 0;
  if (current_slot >= kAgnosticSlots && !is_fee_rate_slot) {
    off = (current_slot - kAgnosticSlots) % kPerCurrencySlots;
  }
  return kAgnosticSlots + kPerCurrencySlots * new_currency_index + off;
}

// api_id → slot. For per-currency screens (moscow / price / mcap), lands
// on the `preferred_currency_index` stride — typically the currently-
// displayed currency so body-first POST /api/show/screen {"s":20}
// (Ticker) keeps the
// user on whichever currency they were already viewing. Returns -1 when
// the api_id isn't in the active rotation.
inline int SlotForApiId(int api_id, std::size_t currency_count,
                        std::size_t preferred_currency_index = 0) {
  if (currency_count == 0) return -1;
  if (preferred_currency_index >= currency_count) {
    preferred_currency_index = 0;
  }
  const std::size_t total = SlotCount(currency_count);
  switch (api_id) {
    case kApiIdBlockHeight:
      return 0;
    case kApiIdClock:
      return 1;
    case kApiIdHalving:
      return 2;
    case kApiIdBitcoinSupply:
      return 3;
    case kApiIdMiningPoolHashrate:
      return 4;
    case kApiIdMiningPoolEarnings:
      return 5;
    case kApiIdBitaxeHashrate:
      return 6;
    case kApiIdBitaxeBestDiff:
      return 7;
    case kApiIdNwcBalance:
      return 8;
    case kApiIdBlockFeeRate:
      return static_cast<int>(total - 1);
    case kApiIdMoscowTime:
      return static_cast<int>(kAgnosticSlots +
                              kPerCurrencySlots * preferred_currency_index);
    case kApiIdBtcPrice:
      return static_cast<int>(kAgnosticSlots +
                              kPerCurrencySlots * preferred_currency_index + 1);
    case kApiIdMarketCap:
      return static_cast<int>(kAgnosticSlots +
                              kPerCurrencySlots * preferred_currency_index + 2);
    default:
      return -1;
  }
}

}  // namespace slot_map
}  // namespace btclock
