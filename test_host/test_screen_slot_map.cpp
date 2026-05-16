// Host tests for app/screen_slot_map.hpp — the pure-logic api_id ↔ slot
// translation used by POST /api/show/screen and /api/status's
// `currentScreen` field. The mapping must track
// ScreenManager::current_kind() exactly; these tests lock that contract
// without linking the renderer / FreeRTOS.
//
// Slot literals for the agnostic prefix (0..kAgnosticSlots-1) are kept
// as bare integers: those positions ARE the contract — bumping a slot
// by inserting a new agnostic kind in the middle is supposed to break
// the test. Per-currency and fee-rate positions derive from
// kAgnosticSlots / kPerCurrencySlots / SlotCount so a kAgnosticSlots
// bump doesn't ripple through every assertion.

#include <initializer_list>

#include "app/screen_slot_map.hpp"
#include "doctest.h"

namespace sm = btclock::slot_map;

namespace {

// Derived helpers — `Slot(...)` reads as "the BtcPrice slot for the
// second currency under a 4-currency layout".
constexpr std::size_t Slot(int api_id, std::size_t currency_count,
                           std::size_t pref = 0) {
  return static_cast<std::size_t>(
      sm::SlotForApiId(api_id, currency_count, pref));
}

// Locally-scoped formula for the trailing fee-rate slot. Same as
// sm::SlotCount(C) - 1, just spelled out for readability where the
// slot is named (rather than counted).
constexpr std::size_t FeeSlot(std::size_t currency_count) {
  return sm::SlotCount(currency_count) - 1;
}

}  // namespace

TEST_CASE("SlotCount follows kAgnosticSlots + kPerCurrencySlots*C + 1") {
  // Formula contract: agnostic prefix + per-currency block + trailing
  // fee-rate singleton. The asserted side is derived from the same
  // constants so a kAgnosticSlots bump auto-propagates; the test still
  // catches drift in the *shape* of the formula (e.g. losing the +1
  // for the fee slot, or doubling the per-currency stride).
  for (std::size_t C :
       {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{7}}) {
    CAPTURE(C);
    CHECK(sm::SlotCount(C) ==
          sm::kAgnosticSlots + sm::kPerCurrencySlots * C + 1);
  }
}

TEST_CASE("ApiIdForSlot covers the agnostic prefix") {
  // Slot 0..8 are the stable agnostic prefix — these literals ARE the
  // contract. A new agnostic kind inserted *before* one of these is
  // supposed to break the test (slot index shifted).
  CHECK(sm::ApiIdForSlot(0, 4) == sm::kApiIdBlockHeight);
  CHECK(sm::ApiIdForSlot(1, 4) == sm::kApiIdClock);
  CHECK(sm::ApiIdForSlot(2, 4) == sm::kApiIdHalving);
  CHECK(sm::ApiIdForSlot(3, 4) == sm::kApiIdBitcoinSupply);
  CHECK(sm::ApiIdForSlot(4, 4) == sm::kApiIdMiningPoolHashrate);
  CHECK(sm::ApiIdForSlot(5, 4) == sm::kApiIdMiningPoolEarnings);
  CHECK(sm::ApiIdForSlot(6, 4) == sm::kApiIdBitaxeHashrate);
  CHECK(sm::ApiIdForSlot(7, 4) == sm::kApiIdBitaxeBestDiff);
  CHECK(sm::ApiIdForSlot(8, 4) == sm::kApiIdNwcBalance);
}

TEST_CASE(
    "ApiIdForSlot covers MiningPoolEstimatedEarnings as the last agnostic "
    "slot") {
  // Slot 9 = kApiIdMiningPoolEstimatedEarnings — sits at the *end* of the
  // agnostic block (after NwcBalance) so the screen's introduction
  // didn't shift bitaxe / NWC slot indices. New screens added later
  // should follow the same pattern.
  CHECK(sm::ApiIdForSlot(9, 4) == sm::kApiIdMiningPoolEstimatedEarnings);
}

TEST_CASE("ApiIdForSlot walks the per-currency stride") {
  // 4 currencies → kAgnosticSlots..kAgnosticSlots+11 cycle the
  // (Moscow, Price, MCap) triple four times. Indices derived from
  // kAgnosticSlots so a bump doesn't break this assertion.
  const std::size_t base = sm::kAgnosticSlots;
  CHECK(sm::ApiIdForSlot(base + 0, 4) == sm::kApiIdMoscowTime);
  CHECK(sm::ApiIdForSlot(base + 1, 4) == sm::kApiIdBtcPrice);
  CHECK(sm::ApiIdForSlot(base + 2, 4) == sm::kApiIdMarketCap);
  CHECK(sm::ApiIdForSlot(base + 3, 4) == sm::kApiIdMoscowTime);  // ccy 1
  CHECK(sm::ApiIdForSlot(base + 5, 4) == sm::kApiIdMarketCap);   // ccy 1
  CHECK(sm::ApiIdForSlot(base + 11, 4) == sm::kApiIdMarketCap);  // ccy 3
}

TEST_CASE("ApiIdForSlot recognises the trailing fee-rate singleton") {
  CHECK(sm::ApiIdForSlot(FeeSlot(4), 4) == sm::kApiIdBlockFeeRate);
  CHECK(sm::ApiIdForSlot(FeeSlot(2), 2) == sm::kApiIdBlockFeeRate);
  CHECK(sm::ApiIdForSlot(FeeSlot(1), 1) == sm::kApiIdBlockFeeRate);
}

TEST_CASE("ApiIdForSlot returns -1 for out-of-range slots") {
  CHECK(sm::ApiIdForSlot(sm::SlotCount(4), 4) == -1);
  CHECK(sm::ApiIdForSlot(100, 1) == -1);
}

TEST_CASE("SlotForApiId rejects unknown api_ids") {
  CHECK(sm::SlotForApiId(7, 4, 0) == -1);  // 7 isn't allocated
  CHECK(sm::SlotForApiId(-1, 4, 0) == -1);
  CHECK(sm::SlotForApiId(999, 4, 0) == -1);
}

TEST_CASE("SlotForApiId maps the agnostic prefix stably") {
  CHECK(sm::SlotForApiId(sm::kApiIdBlockHeight, 4, 0) == 0);
  CHECK(sm::SlotForApiId(sm::kApiIdClock, 4, 0) == 1);
  CHECK(sm::SlotForApiId(sm::kApiIdHalving, 4, 0) == 2);
  CHECK(sm::SlotForApiId(sm::kApiIdBitcoinSupply, 4, 0) == 3);
  CHECK(sm::SlotForApiId(sm::kApiIdMiningPoolHashrate, 4, 0) == 4);
  CHECK(sm::SlotForApiId(sm::kApiIdMiningPoolEarnings, 4, 0) == 5);
  CHECK(sm::SlotForApiId(sm::kApiIdBitaxeHashrate, 4, 0) == 6);
  CHECK(sm::SlotForApiId(sm::kApiIdBitaxeBestDiff, 4, 0) == 7);
  CHECK(sm::SlotForApiId(sm::kApiIdNwcBalance, 4, 0) == 8);
  CHECK(sm::SlotForApiId(sm::kApiIdMiningPoolEstimatedEarnings, 4, 0) == 9);
}

TEST_CASE("SlotForApiId lands on the preferred currency index") {
  // Per-currency screens with a preferred currency of 2 (third slot of 4).
  // Base slot = kAgnosticSlots + kPerCurrencySlots * 2.
  const std::size_t base = sm::kAgnosticSlots + sm::kPerCurrencySlots * 2;
  CHECK(sm::SlotForApiId(sm::kApiIdMoscowTime, 4, 2) ==
        static_cast<int>(base + 0));
  CHECK(sm::SlotForApiId(sm::kApiIdBtcPrice, 4, 2) ==
        static_cast<int>(base + 1));
  CHECK(sm::SlotForApiId(sm::kApiIdMarketCap, 4, 2) ==
        static_cast<int>(base + 2));
}

TEST_CASE("SlotForApiId clamps an out-of-range preferred_currency_index") {
  // Garbage preferred index falls back to currencies[0] instead of
  // producing an out-of-range slot.
  CHECK(sm::SlotForApiId(sm::kApiIdMoscowTime, 4, 99) ==
        static_cast<int>(sm::kAgnosticSlots));
  CHECK(sm::SlotForApiId(sm::kApiIdBtcPrice, 4, 99) ==
        static_cast<int>(sm::kAgnosticSlots + 1));
}

TEST_CASE("SlotForApiId for fee-rate lands on the trailing singleton") {
  CHECK(sm::SlotForApiId(sm::kApiIdBlockFeeRate, 4, 0) ==
        static_cast<int>(FeeSlot(4)));
  CHECK(sm::SlotForApiId(sm::kApiIdBlockFeeRate, 2, 0) ==
        static_cast<int>(FeeSlot(2)));
  CHECK(sm::SlotForApiId(sm::kApiIdBlockFeeRate, 1, 0) ==
        static_cast<int>(FeeSlot(1)));
}

TEST_CASE("SlotForApiId returns -1 when there are no active currencies") {
  // SlotForApiId assumes the rotation has at least one currency. A caller
  // with an empty list gets -1 rather than a nonsensical clamp — the
  // HTTP handler responds 400 in that case.
  CHECK(sm::SlotForApiId(sm::kApiIdBlockHeight, 0, 0) == -1);
  CHECK(sm::SlotForApiId(sm::kApiIdBlockFeeRate, 0, 0) == -1);
}

TEST_CASE("Round-trip: slot -> api_id -> slot holds for agnostic + fee") {
  const std::size_t C = 4;
  // Sweep every agnostic slot + the fee-rate trailing slot; round-trip
  // each through ApiIdForSlot → SlotForApiId and expect the same slot
  // back (pref=0 since agnostic slots ignore the currency hint).
  for (std::size_t slot = 0; slot < sm::kAgnosticSlots; ++slot) {
    const int api_id = sm::ApiIdForSlot(slot, C);
    CAPTURE(slot);
    CAPTURE(api_id);
    const int back =
        sm::SlotForApiId(api_id, C, /*preferred_currency_index=*/0);
    CHECK(back == static_cast<int>(slot));
  }
  {
    const std::size_t slot = FeeSlot(C);
    const int api_id = sm::ApiIdForSlot(slot, C);
    CAPTURE(slot);
    CAPTURE(api_id);
    const int back = sm::SlotForApiId(api_id, C, 0);
    CHECK(back == static_cast<int>(slot));
  }
}

TEST_CASE(
    "Round-trip: per-currency slot round-trips through the preferred index") {
  const std::size_t C = 4;
  // Iterate every per-currency slot; round-tripping with the currency
  // index derived from the slot must recover the original slot.
  for (std::size_t slot = sm::kAgnosticSlots; slot < FeeSlot(C); ++slot) {
    const int api_id = sm::ApiIdForSlot(slot, C);
    const std::size_t pref =
        (slot - sm::kAgnosticSlots) / sm::kPerCurrencySlots;
    CAPTURE(slot);
    CAPTURE(api_id);
    CAPTURE(pref);
    const int back = sm::SlotForApiId(api_id, C, pref);
    CHECK(back == static_cast<int>(slot));
  }
}

TEST_CASE(
    "TransposeSlotToCurrency preserves per-currency kind across currencies") {
  // 4 currencies → per-currency block starts at kAgnosticSlots, stride 3.
  const std::size_t C = 4;
  // Market Cap USD → Market Cap EUR.
  CHECK(sm::TransposeSlotToCurrency(Slot(sm::kApiIdMarketCap, C, 0),
                                    /*new_currency_index=*/1,
                                    C) == Slot(sm::kApiIdMarketCap, C, 1));
  // Moscow Time JPY → Moscow Time GBP.
  CHECK(sm::TransposeSlotToCurrency(Slot(sm::kApiIdMoscowTime, C, 3),
                                    /*new_currency_index=*/2,
                                    C) == Slot(sm::kApiIdMoscowTime, C, 2));
  // Price EUR → Price USD.
  CHECK(sm::TransposeSlotToCurrency(Slot(sm::kApiIdBtcPrice, C, 1),
                                    /*new_currency_index=*/0,
                                    C) == Slot(sm::kApiIdBtcPrice, C, 0));
}

TEST_CASE(
    "TransposeSlotToCurrency lands on Moscow of new currency from agnostic "
    "slots") {
  // Agnostic slots have no per-currency offset to carry — default to
  // off=0 (Moscow) so auto-rotate walks Moscow → Price → MarketCap next.
  const std::size_t C = 4;
  for (std::size_t agnostic = 0; agnostic < sm::kAgnosticSlots; ++agnostic) {
    CAPTURE(agnostic);
    CHECK(sm::TransposeSlotToCurrency(agnostic, 2, C) ==
          Slot(sm::kApiIdMoscowTime, C, 2));
  }
}

TEST_CASE("TransposeSlotToCurrency from fee-rate slot lands on Moscow") {
  const std::size_t C = 4;
  const std::size_t fee = FeeSlot(C);
  // Fee rate is agnostic-ish — no offset to carry.
  CHECK(sm::TransposeSlotToCurrency(fee, 1, C) ==
        Slot(sm::kApiIdMoscowTime, C, 1));
  CHECK(sm::TransposeSlotToCurrency(fee, 3, C) ==
        Slot(sm::kApiIdMoscowTime, C, 3));
}

TEST_CASE(
    "TransposeSlotToCurrency is a no-op when target currency == current") {
  const std::size_t C = 4;
  // Market Cap EUR asked to switch to EUR stays.
  CHECK(sm::TransposeSlotToCurrency(Slot(sm::kApiIdMarketCap, C, 1), 1, C) ==
        Slot(sm::kApiIdMarketCap, C, 1));
  // Moscow Time USD asked to stay on USD.
  CHECK(sm::TransposeSlotToCurrency(Slot(sm::kApiIdMoscowTime, C, 0), 0, C) ==
        Slot(sm::kApiIdMoscowTime, C, 0));
}

TEST_CASE(
    "Regression bd btclock_v4-oni: slot_map and ScreenManager agree on "
    "kAgnosticSlots — fee-rate slot lands on its own kind, not OOB") {
  // Pin the consumer contract that the kAgnosticSlots fix re-established:
  //
  //   slot_map::ApiIdForSlot(slot_count-1, C) == kApiIdBlockFeeRate
  //   slot_map::ApiIdForSlot(kAgnosticSlots, C) == kApiIdMoscowTime
  //   slot_map::ApiIdForSlot(kAgnosticSlots+1, C) == kApiIdBtcPrice
  //   slot_map::ApiIdForSlot(kAgnosticSlots+2, C) == kApiIdMarketCap
  //
  // The ScreenManager.cpp KindForSlot switch carries a static_assert
  // pinned to slot_map::kAgnosticSlots, so a divergence (slot_map grows
  // by one without the agnostic switch being extended) is a compile
  // error there. From this side we just verify the slot_map invariants
  // every consumer relies on, across the currency-count axis where the
  // bug initially surfaced (C=1 fee slot landed in a per-currency OOB).
  for (std::size_t C = 1; C <= 7; ++C) {
    CAPTURE(C);
    CHECK(sm::ApiIdForSlot(FeeSlot(C), C) == sm::kApiIdBlockFeeRate);
    CHECK(sm::ApiIdForSlot(sm::kAgnosticSlots, C) == sm::kApiIdMoscowTime);
    CHECK(sm::ApiIdForSlot(sm::kAgnosticSlots + 1, C) == sm::kApiIdBtcPrice);
    CHECK(sm::ApiIdForSlot(sm::kAgnosticSlots + 2, C) == sm::kApiIdMarketCap);
    // NWC balance is in the agnostic prefix — must NOT slip into the
    // per-currency stride. Pre-fix, ScreenManager.kAgnosticSlots=8 made
    // current_currency() return currencies_[0] for slot 8 (NWC), and
    // KindForSlot(slot_count-1) hit the per-currency arithmetic with an
    // OOB ccy_idx on the fee slot.
    CHECK(sm::ApiIdForSlot(8, C) == sm::kApiIdNwcBalance);
  }
}
