// Host tests for app/screen_slot_map.hpp — the pure-logic api_id ↔ slot
// translation used by POST /api/show/screen and /api/status's
// `currentScreen` field. The mapping must track
// ScreenManager::current_kind() exactly; these tests lock that contract
// without linking the renderer / FreeRTOS.

#include "doctest.h"

#include <initializer_list>

#include "app/screen_slot_map.hpp"

namespace sm = btclock::slot_map;

TEST_CASE("SlotCount matches 8 + 3C + 1") {
  // 8 currency-agnostic slots (block, clock, halving, supply,
  // mining-pool-hashrate, mining-pool-earnings, bitaxe-hashrate,
  // bitaxe-best-diff) + 3 per-currency slots + 1 trailing fee-rate slot.
  CHECK(sm::SlotCount(1) == 12);
  CHECK(sm::SlotCount(2) == 15);
  CHECK(sm::SlotCount(4) == 21);  // the Rev B default (USD/EUR/GBP/JPY)
}

TEST_CASE("ApiIdForSlot covers the agnostic prefix") {
  CHECK(sm::ApiIdForSlot(0, 4) == sm::kApiIdBlockHeight);
  CHECK(sm::ApiIdForSlot(1, 4) == sm::kApiIdClock);
  CHECK(sm::ApiIdForSlot(2, 4) == sm::kApiIdHalving);
  CHECK(sm::ApiIdForSlot(3, 4) == sm::kApiIdBitcoinSupply);
  CHECK(sm::ApiIdForSlot(4, 4) == sm::kApiIdMiningPoolHashrate);
  CHECK(sm::ApiIdForSlot(5, 4) == sm::kApiIdMiningPoolEarnings);
  CHECK(sm::ApiIdForSlot(6, 4) == sm::kApiIdBitaxeHashrate);
  CHECK(sm::ApiIdForSlot(7, 4) == sm::kApiIdBitaxeBestDiff);
}

TEST_CASE("ApiIdForSlot walks the per-currency stride") {
  // With 4 currencies: slots 8..19 cycle (moscow, price, mcap) four times.
  CHECK(sm::ApiIdForSlot(8, 4)  == sm::kApiIdMoscowTime);
  CHECK(sm::ApiIdForSlot(9, 4)  == sm::kApiIdBtcPrice);
  CHECK(sm::ApiIdForSlot(10, 4) == sm::kApiIdMarketCap);
  CHECK(sm::ApiIdForSlot(11, 4) == sm::kApiIdMoscowTime);   // currencies[1]
  CHECK(sm::ApiIdForSlot(13, 4) == sm::kApiIdMarketCap);    // currencies[1]
  CHECK(sm::ApiIdForSlot(19, 4) == sm::kApiIdMarketCap);    // currencies[3]
}

TEST_CASE("ApiIdForSlot recognises the trailing fee-rate singleton") {
  CHECK(sm::ApiIdForSlot(20, 4) == sm::kApiIdBlockFeeRate);
  CHECK(sm::ApiIdForSlot(14, 2) == sm::kApiIdBlockFeeRate);
  CHECK(sm::ApiIdForSlot(11, 1) == sm::kApiIdBlockFeeRate);
}

TEST_CASE("ApiIdForSlot returns -1 for out-of-range slots") {
  CHECK(sm::ApiIdForSlot(21, 4) == -1);
  CHECK(sm::ApiIdForSlot(100, 1) == -1);
}

TEST_CASE("SlotForApiId rejects unknown api_ids") {
  CHECK(sm::SlotForApiId(7, 4, 0) == -1);   // 7 isn't allocated
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
}

TEST_CASE("SlotForApiId lands on the preferred currency index") {
  // Per-currency screens with a preferred currency of 2 (third slot of 4).
  // Base slot = kAgnosticSlots (8) + kPerCurrencySlots (3) * 2 = 14.
  CHECK(sm::SlotForApiId(sm::kApiIdMoscowTime, 4, 2) == 14);
  CHECK(sm::SlotForApiId(sm::kApiIdBtcPrice,   4, 2) == 15);
  CHECK(sm::SlotForApiId(sm::kApiIdMarketCap,  4, 2) == 16);
}

TEST_CASE("SlotForApiId clamps an out-of-range preferred_currency_index") {
  // Garbage preferred index falls back to currencies[0] instead of
  // producing an out-of-range slot.
  CHECK(sm::SlotForApiId(sm::kApiIdMoscowTime, 4, 99) == 8);
  CHECK(sm::SlotForApiId(sm::kApiIdBtcPrice,   4, 99) == 9);
}

TEST_CASE("SlotForApiId for fee-rate lands on the trailing singleton") {
  CHECK(sm::SlotForApiId(sm::kApiIdBlockFeeRate, 4, 0) == 20);
  CHECK(sm::SlotForApiId(sm::kApiIdBlockFeeRate, 2, 0) == 14);
  CHECK(sm::SlotForApiId(sm::kApiIdBlockFeeRate, 1, 0) == 11);
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
  const std::size_t total = sm::SlotCount(C);
  for (std::size_t slot : {std::size_t{0}, std::size_t{1}, std::size_t{2},
                           std::size_t{3}, std::size_t{4}, std::size_t{5},
                           std::size_t{6}, std::size_t{7},
                           total - 1}) {
    const int api_id = sm::ApiIdForSlot(slot, C);
    CAPTURE(slot);
    CAPTURE(api_id);
    const int back = sm::SlotForApiId(api_id, C, /*preferred=*/0);
    CHECK(back == static_cast<int>(slot));
  }
}

TEST_CASE("Round-trip: per-currency slot round-trips through the preferred index") {
  const std::size_t C = 4;
  // Iterate every per-currency slot; round-tripping with the currency
  // index derived from the slot must recover the original slot.
  for (std::size_t slot = sm::kAgnosticSlots; slot < sm::SlotCount(C) - 1;
       ++slot) {
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

TEST_CASE("TransposeSlotToCurrency preserves per-currency kind across currencies") {
  // 4 currencies → per-currency block starts at 8, stride 3.
  const std::size_t C = 4;
  // Market Cap USD (slot 10, ccy 0, off 2) → Market Cap EUR (ccy 1, off 2) = 13.
  CHECK(sm::TransposeSlotToCurrency(/*current=*/10, /*new_ccy=*/1, C) == 13);
  // Moscow Time JPY (slot 17, ccy 3, off 0) → Moscow Time GBP (ccy 2, off 0) = 14.
  CHECK(sm::TransposeSlotToCurrency(/*current=*/17, /*new_ccy=*/2, C) == 14);
  // Price EUR (slot 12, ccy 1, off 1) → Price USD (ccy 0, off 1) = 9.
  CHECK(sm::TransposeSlotToCurrency(/*current=*/12, /*new_ccy=*/0, C) == 9);
}

TEST_CASE("TransposeSlotToCurrency lands on Moscow of new currency from agnostic slots") {
  // Agnostic slots have no per-currency offset to carry — default to
  // off=0 (Moscow) so auto-rotate walks Moscow → Price → MarketCap next.
  const std::size_t C = 4;
  for (std::size_t agnostic : {std::size_t{0}, std::size_t{1},
                                std::size_t{2}, std::size_t{3},
                                std::size_t{4}, std::size_t{5},
                                std::size_t{6}, std::size_t{7}}) {
    CAPTURE(agnostic);
    // New currency index 2 → Moscow slot = 8 + 3*2 = 14.
    CHECK(sm::TransposeSlotToCurrency(agnostic, 2, C) == 14);
  }
}

TEST_CASE("TransposeSlotToCurrency from fee-rate slot lands on Moscow") {
  const std::size_t C = 4;
  const std::size_t fee_slot = sm::SlotCount(C) - 1;  // 20
  // Fee rate is agnostic-ish — no offset to carry.
  CHECK(sm::TransposeSlotToCurrency(fee_slot, 1, C) == 11);  // Moscow EUR
  CHECK(sm::TransposeSlotToCurrency(fee_slot, 3, C) == 17);  // Moscow JPY
}

TEST_CASE("TransposeSlotToCurrency is a no-op when target currency == current") {
  const std::size_t C = 4;
  // Market Cap EUR (slot 13, ccy 1) asked to switch to EUR (ccy 1) stays.
  CHECK(sm::TransposeSlotToCurrency(13, 1, C) == 13);
  // Moscow Time USD (slot 8, ccy 0) asked to stay on USD.
  CHECK(sm::TransposeSlotToCurrency(8, 0, C) == 8);
}
