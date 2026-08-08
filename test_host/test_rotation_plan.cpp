// Host tests for app/rotation_plan.hpp — pure-logic builder that turns
// the `screenOrder` NVS CSV + `screen<id>Visible` toggles + active
// currency list into the ScreenManager's auto-rotate traversal sequence.
//
// These tests pin the two Rev B bugs (actCurrencies ignored + screenOrder
// ignored) at the building-block layer before the ScreenManager tests
// exercise the composed behaviour.
//
// Slot expectations are spelled in terms of which *screen* sits there
// (via `Slot(kApiIdXxx, C, pref)`) rather than the integer index. That
// way, a future kAgnosticSlots bump shifts every per-currency slot
// without rippling through dozens of test literals — the slot_map
// switch table moves and the tests update automatically.

#include <optional>
#include <string>
#include <vector>

#include "app/rotation_plan.hpp"
#include "app/screen_slot_map.hpp"
#include "doctest.h"

namespace rp = btclock::rotation_plan;
namespace sm = btclock::slot_map;

namespace {

// Default "everything enabled" predicate — the plan builder still honours
// screenOrder and per-currency expansion without any disabled screens.
const auto kAllEnabled = [](int) { return true; };

// Shorthand: `Slot(kApiIdBtcPrice, 4, 1)` reads as "the BtcPrice slot
// for the second currency under a 4-currency layout". Returns size_t
// to compare cleanly against rotation_sequence_ entries.
constexpr std::size_t Slot(int api_id, std::size_t currency_count,
                           std::size_t pref = 0) {
  return static_cast<std::size_t>(
      sm::SlotForApiId(api_id, currency_count, pref));
}

}  // namespace

TEST_CASE("empty screenOrder falls back to full slot_map index order") {
  // 2 currencies → kAgnosticSlots + 2*kPerCurrencySlots + 1 slots.
  // Test pins that the fallback sequence covers every slot in order;
  // the absolute count is derived from slot_map so a layout bump
  // doesn't touch this assertion.
  const auto seq = rp::BuildRotationSequence("", kAllEnabled, 2);
  REQUIRE(seq.size() == sm::SlotCount(2));
  for (std::size_t i = 0; i < seq.size(); ++i) CHECK(seq[i] == i);
}

TEST_CASE("per-currency kinds expand to one slot per active currency") {
  // Just BtcPrice with 2 currencies → BtcPrice for ccy 0 + ccy 1.
  const auto seq = rp::BuildRotationSequence("20", kAllEnabled, 2);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == Slot(sm::kApiIdBtcPrice, 2, 0));
  CHECK(seq[1] == Slot(sm::kApiIdBtcPrice, 2, 1));
}

TEST_CASE("Bug 1: screenOrder=20 with 4 currencies emits 4 BtcPrice slots") {
  // Pins that per-currency expansion walks every entry in the currencies
  // list when the user has a single BtcPrice in screenOrder. Protects
  // against a future refactor that accidentally stops at currencies[0].
  const auto seq = rp::BuildRotationSequence("20", kAllEnabled, 4);
  REQUIRE(seq.size() == 4);
  CHECK(seq[0] == Slot(sm::kApiIdBtcPrice, 4, 0));  // USD
  CHECK(seq[1] == Slot(sm::kApiIdBtcPrice, 4, 1));  // EUR
  CHECK(seq[2] == Slot(sm::kApiIdBtcPrice, 4, 2));  // GBP
  CHECK(seq[3] == Slot(sm::kApiIdBtcPrice, 4, 3));  // JPY
}

TEST_CASE(
    "Bug 1 root cause: rotation respects currency_count, never walks GBP "
    "with actCurrencies=[USD,EUR]") {
  // If ScreenManager is constructed with `currencies = ["USD","EUR"]`
  // (2 entries), the BuildRotationSequence cannot emit a slot for GBP —
  // even when screenOrder asks for MarketCap (api_id 30). 2 entries max.
  const auto seq = rp::BuildRotationSequence("30", kAllEnabled, 2);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == Slot(sm::kApiIdMarketCap, 2, 0));  // USD
  CHECK(seq[1] == Slot(sm::kApiIdMarketCap, 2, 1));  // EUR
  // Would-be GBP MarketCap slot under a 3-currency layout — must not
  // appear under the 2-currency rotation.
  const std::size_t gbp_mcap_3ccy = Slot(sm::kApiIdMarketCap, 3, 2);
  for (const auto s : seq) CHECK(s != gbp_mcap_3ccy);
}

TEST_CASE("Bug 2: screenOrder drives traversal, not slot_map index order") {
  // User's rotation: BlockHeight (0) → BtcPrice (20) → MoscowTime (10)
  // with 2 currencies. Expected: 1 BlockHeight slot + 2 BtcPrice slots
  // + 2 MoscowTime slots.
  const auto seq = rp::BuildRotationSequence("0,20,10", kAllEnabled, 2);
  REQUIRE(seq.size() == 5);
  CHECK(seq[0] == Slot(sm::kApiIdBlockHeight, 2));
  CHECK(seq[1] == Slot(sm::kApiIdBtcPrice, 2, 0));    // USD
  CHECK(seq[2] == Slot(sm::kApiIdBtcPrice, 2, 1));    // EUR
  CHECK(seq[3] == Slot(sm::kApiIdMoscowTime, 2, 0));  // USD
  CHECK(seq[4] == Slot(sm::kApiIdMoscowTime, 2, 1));  // EUR
}

TEST_CASE(
    "Bug 2: disabled screen is dropped from rotation even when in "
    "screenOrder") {
  // User ordered [BlockHeight, Clock, Halving] but has Clock disabled.
  auto enabled = [](int api_id) { return api_id != sm::kApiIdClock; };
  const auto seq = rp::BuildRotationSequence("0,3,4", enabled, 1);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == Slot(sm::kApiIdBlockHeight, 1));
  CHECK(seq[1] == Slot(sm::kApiIdHalving, 1));
}

TEST_CASE(
    "Bug 2 task-spec shape: [0, 20, 10] with screen 3 disabled walks "
    "BlockHeight → BtcPrice ×C → MoscowTime ×C") {
  // Exact shape called out in the bug report. Clock (api_id 3) disabled.
  // 4 currencies expands per-currency kinds to 4 entries each.
  auto enabled = [](int api_id) { return api_id != sm::kApiIdClock; };
  const auto seq = rp::BuildRotationSequence("0,20,10", enabled, 4);
  // 1 (block) + 4 (price) + 4 (moscow) = 9.
  REQUIRE(seq.size() == 9);
  CHECK(seq[0] == Slot(sm::kApiIdBlockHeight, 4));
  CHECK(seq[1] == Slot(sm::kApiIdBtcPrice, 4, 0));
  CHECK(seq[2] == Slot(sm::kApiIdBtcPrice, 4, 1));
  CHECK(seq[3] == Slot(sm::kApiIdBtcPrice, 4, 2));
  CHECK(seq[4] == Slot(sm::kApiIdBtcPrice, 4, 3));
  CHECK(seq[5] == Slot(sm::kApiIdMoscowTime, 4, 0));
  CHECK(seq[6] == Slot(sm::kApiIdMoscowTime, 4, 1));
  CHECK(seq[7] == Slot(sm::kApiIdMoscowTime, 4, 2));
  CHECK(seq[8] == Slot(sm::kApiIdMoscowTime, 4, 3));
  const std::size_t clock = Slot(sm::kApiIdClock, 4);
  for (const auto s : seq) CHECK(s != clock);
}

TEST_CASE("fee-rate slot appears when the user's screenOrder keeps api_id 6") {
  // Fee rate is a trailing singleton — its slot is SlotCount(C)-1.
  const auto seq = rp::BuildRotationSequence("0,6", kAllEnabled, 2);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == Slot(sm::kApiIdBlockHeight, 2));
  CHECK(seq[1] == Slot(sm::kApiIdBlockFeeRate, 2));
}

TEST_CASE(
    "sequence never wedges: all-disabled order falls back to a single slot") {
  auto enabled = [](int) { return false; };
  const auto seq = rp::BuildRotationSequence("0,3,4", enabled, 2);
  REQUIRE(seq.size() == 1);
  // Arbitrary choice — documented as "slot 0" in the builder. Asserted
  // here so a future refactor can't silently return an empty vector,
  // which would divide-by-zero in the rotation index math.
  CHECK(seq[0] == Slot(sm::kApiIdBlockHeight, 2));
}

TEST_CASE("unknown api_ids in screenOrder are silently skipped") {
  // `9999` isn't a registered api_id — drops from the sequence without
  // failing the whole parse.
  const auto seq = rp::BuildRotationSequence("0,9999,4", kAllEnabled, 1);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == Slot(sm::kApiIdBlockHeight, 1));
  CHECK(seq[1] == Slot(sm::kApiIdHalving, 1));
}

TEST_CASE("non-numeric tokens in screenOrder are silently skipped") {
  // Defensive: a stray token (e.g. from a corrupted NVS blob or a future
  // firmware that writes extra metadata) must not wedge the parser.
  const auto seq = rp::BuildRotationSequence("0,abc,,4", kAllEnabled, 1);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == Slot(sm::kApiIdBlockHeight, 1));
  CHECK(seq[1] == Slot(sm::kApiIdHalving, 1));
}

TEST_CASE(
    "backwards compat: empty screenOrder walks every slot (pre-feature "
    "devices upgrade cleanly)") {
  // With 4 currencies, walk every slot in slot_map index order. Count
  // derives from slot_map so a kAgnosticSlots bump doesn't touch this.
  const auto seq = rp::BuildRotationSequence("", kAllEnabled, 4);
  REQUIRE(seq.size() == sm::SlotCount(4));
  for (std::size_t i = 0; i < seq.size(); ++i) CHECK(seq[i] == i);
}

TEST_CASE(
    "cold-boot fallback honours is_enabled — disabled feature slots "
    "never appear at first boot") {
  // Repro for the "fresh device shows mining-pool / bitaxe screens
  // even with miningPoolStats=false and bitaxeEnabled=false" report.
  // Predicate gates ids 70/71/72 (mining pool family) + 80/81 (bitaxe),
  // as the is_enabled lambdas in init_screen_manager / event_loop now do.
  auto enabled = [](int api_id) {
    if (api_id == sm::kApiIdMiningPoolHashrate ||
        api_id == sm::kApiIdMiningPoolEarnings ||
        api_id == sm::kApiIdMiningPoolEstimatedEarnings)
      return false;
    if (api_id == sm::kApiIdBitaxeHashrate ||
        api_id == sm::kApiIdBitaxeBestDiff)
      return false;
    return true;
  };
  const auto seq = rp::BuildRotationSequence("", enabled, 1);
  // BlockHeight + Clock + Halving + BitcoinSupply + NwcBalance +
  // BlockHeightSplit kept; mining-pool family + bitaxe family dropped;
  // per-currency (Moscow, Price, MarketCap) for the one currency;
  // trailing BlockFeeRate.
  REQUIRE(seq.size() == 10);
  CHECK(seq[0] == Slot(sm::kApiIdBlockHeight, 1));
  CHECK(seq[1] == Slot(sm::kApiIdClock, 1));
  CHECK(seq[2] == Slot(sm::kApiIdHalving, 1));
  CHECK(seq[3] == Slot(sm::kApiIdBitcoinSupply, 1));
  // NwcBalance is included — cold-boot predicate doesn't gate it here
  // (the production predicate checks nwc.enabled — exercised in
  // test_screen_rotation).
  CHECK(seq[4] == Slot(sm::kApiIdNwcBalance, 1));
  CHECK(seq[5] == Slot(sm::kApiIdBlockHeightSplit, 1));
  CHECK(seq[6] == Slot(sm::kApiIdMoscowTime, 1));
  CHECK(seq[7] == Slot(sm::kApiIdBtcPrice, 1));
  CHECK(seq[8] == Slot(sm::kApiIdMarketCap, 1));
  CHECK(seq[9] == Slot(sm::kApiIdBlockFeeRate, 1));
  const std::size_t gated[] = {
      Slot(sm::kApiIdMiningPoolHashrate, 1),
      Slot(sm::kApiIdMiningPoolEarnings, 1),
      Slot(sm::kApiIdBitaxeHashrate, 1),
      Slot(sm::kApiIdBitaxeBestDiff, 1),
      Slot(sm::kApiIdMiningPoolEstimatedEarnings, 1),
  };
  for (const auto s : seq) {
    for (const auto g : gated) CHECK(s != g);
  }
}

// --- ResumeSlot: pin the boot-time cursor restore policy ---
//
// Mirrors `init_screen_manager.cpp`'s resume block (refactored to call
// rotation_plan::ResumeSlot so these tests exercise the same code).
// Each branch the boot path can take has its own case so a future
// refactor can't silently regress to "always boot on block-height" —
// the bug 4d66391 set out to fix.

TEST_CASE("ResumeSlot: no persisted lastSlot lands on first sequence entry") {
  // Fresh device — NVS GetU32 returns the kNoSavedSlot default. Boot
  // should resume on the user's first-in-order screen, not the
  // constructor default of slot 0. With screenOrder=[Time, BlockHeight]
  // (api ids 3, 0) the rotation_sequence is [Clock, BlockHeight], so
  // the first paint must be the Clock slot.
  const std::vector<std::size_t> seq = {Slot(sm::kApiIdClock, 1),
                                        Slot(sm::kApiIdBlockHeight, 1)};
  const auto r = rp::ResumeSlot(rp::kNoSavedSlot, sm::SlotCount(1), seq);
  CHECK(r == std::optional<std::size_t>{Slot(sm::kApiIdClock, 1)});
}

TEST_CASE("ResumeSlot: persisted slot inside rotation is restored") {
  // User was on a non-first slot when the device rebooted — boot must
  // land back on that slot, not on sequence[0].
  const std::size_t saved = Slot(sm::kApiIdMiningPoolHashrate, 1);
  const std::vector<std::size_t> seq = {Slot(sm::kApiIdBlockHeight, 1),
                                        Slot(sm::kApiIdClock, 1), saved,
                                        Slot(sm::kApiIdHalving, 1)};
  const auto r = rp::ResumeSlot(saved, sm::SlotCount(1), seq);
  CHECK(r == std::optional<std::size_t>{saved});
}

TEST_CASE("ResumeSlot: trailing fee-rate slot resumes even without sequence") {
  // The fee-rate slot lives at slot_count - 1 and is *not* part of
  // rotation_sequence_ unless the user keeps api_id 6 in screenOrder.
  // ResumeSlot still restores it because the user explicitly navigated
  // there before the reboot — dropping them onto sequence[0] would
  // surprise them.
  const std::vector<std::size_t> seq = {Slot(sm::kApiIdBlockHeight, 1),
                                        Slot(sm::kApiIdClock, 1),
                                        Slot(sm::kApiIdHalving, 1)};
  const std::size_t slot_count = sm::SlotCount(1);
  const std::size_t fee = Slot(sm::kApiIdBlockFeeRate, 1);
  const auto r = rp::ResumeSlot(fee, slot_count, seq);
  CHECK(r == std::optional<std::size_t>{fee});
}

TEST_CASE(
    "ResumeSlot: persisted slot dropped from rotation falls back to "
    "sequence[0]") {
  // User was on mining-pool earnings before they PATCHed it out. After
  // reboot the rotation_sequence no longer contains that slot — boot
  // must land on the user's first-in-order screen rather than restoring
  // a slot they can no longer reach via Next/Prev.
  const std::vector<std::size_t> seq = {Slot(sm::kApiIdBlockHeight, 1),
                                        Slot(sm::kApiIdClock, 1),
                                        Slot(sm::kApiIdHalving, 1)};
  const std::size_t saved = Slot(sm::kApiIdMiningPoolEarnings, 1);
  const auto r = rp::ResumeSlot(saved, sm::SlotCount(1), seq);
  CHECK(r == std::optional<std::size_t>{seq[0]});
}

TEST_CASE(
    "ResumeSlot: persisted slot >= slot_count falls back to sequence[0]") {
  // A previous boot saved a slot under a 4-currency layout; user then
  // dropped to a 1-currency layout where slot_count is much smaller.
  // The saved index no longer exists — sequence[0] is the safe landing.
  const std::vector<std::size_t> seq = {Slot(sm::kApiIdBlockHeight, 1),
                                        Slot(sm::kApiIdClock, 1),
                                        Slot(sm::kApiIdHalving, 1)};
  // Take a slot only the 4-currency layout has: GBP MarketCap.
  const std::size_t stale_saved = Slot(sm::kApiIdMarketCap, 4, 2);
  REQUIRE(stale_saved >= sm::SlotCount(1));
  const auto r = rp::ResumeSlot(stale_saved, sm::SlotCount(1), seq);
  CHECK(r == std::optional<std::size_t>{seq[0]});
}

TEST_CASE(
    "ResumeSlot: empty sequence yields nullopt so the constructor default "
    "stays put") {
  // Pathological state — rotation builder returned empty (only
  // possible if both screenOrder is empty AND every slot is gated
  // off, and the BuildRotationSequence wedge guard somehow skipped).
  // ResumeSlot returns nullopt → init_screen_manager skips SetSlot
  // and the ScreenManager constructor's slot_=0 stands.
  const auto r = rp::ResumeSlot(rp::kNoSavedSlot, /*slot_count=*/0, {});
  CHECK_FALSE(r.has_value());
}

TEST_CASE("ResumeSlot: kNoSavedSlot with empty sequence yields nullopt") {
  // Same nullopt outcome via the no-saved-slot branch — kNoSavedSlot
  // != fee_slot (slot_count==0) and isn't in the empty sequence, so
  // we fall through to the empty-sequence guard.
  const std::vector<std::size_t> seq;
  const auto r = rp::ResumeSlot(rp::kNoSavedSlot, /*slot_count=*/0, seq);
  CHECK_FALSE(r.has_value());
}

TEST_CASE(
    "ResumeSlot: saved slot 0 (BlockHeight) restores even though it's the "
    "constructor default") {
  // Distinct from "fresh boot" — user explicitly landed on slot 0 and
  // we must not sentinel-collide. kNoSavedSlot is UINT32_MAX so a
  // legitimately-saved 0 is fine, and the in-sequence check matches.
  const std::vector<std::size_t> seq = {Slot(sm::kApiIdHalving, 1),
                                        Slot(sm::kApiIdBlockHeight, 1),
                                        Slot(sm::kApiIdClock, 1)};
  const std::size_t bh = Slot(sm::kApiIdBlockHeight, 1);
  const auto r = rp::ResumeSlot(bh, sm::SlotCount(1), seq);
  CHECK(r == std::optional<std::size_t>{bh});
}

TEST_CASE("cold-boot fallback never wedges when every api_id is gated off") {
  // Pathological config: predicate rejects everything. Builder must
  // still emit at least one slot so ScreenManager's index math doesn't
  // divide by zero — same guarantee as the screenOrder path.
  auto enabled = [](int) { return false; };
  const auto seq = rp::BuildRotationSequence("", enabled, 2);
  REQUIRE(seq.size() == 1);
  CHECK(seq[0] == Slot(sm::kApiIdBlockHeight, 2));
}

// --- Composed behaviour: build-sequence + step-sequence ---
//
// These replay the bug-report scenarios against the same primitives
// ScreenManager uses internally. Each step = one "POST /api/screen/next".

TEST_CASE(
    "Bug 1 fix: MarketCap with actCurrencies=[USD,EUR], Next cycles only "
    "USD+EUR, never GBP/JPY") {
  // actCurrencies=[USD,EUR] → currency_count=2.
  // screenOrder includes MarketCap (30). Press Next repeatedly starting
  // on MarketCap USD — should loop USD → EUR → USD without visiting a
  // GBP slot.
  const auto seq = rp::BuildRotationSequence("30", kAllEnabled, 2);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == Slot(sm::kApiIdMarketCap, 2, 0));
  CHECK(seq[1] == Slot(sm::kApiIdMarketCap, 2, 1));

  auto [slot1, idx1] = rp::StepSequence(seq, 0, +1);
  CHECK(slot1 == Slot(sm::kApiIdMarketCap, 2, 1));  // MCap EUR
  auto [slot2, idx2] = rp::StepSequence(seq, idx1, +1);
  CHECK(slot2 == Slot(sm::kApiIdMarketCap, 2, 0));  // wraps to MCap USD
  // No GBP/JPY slots in a 2-currency config so this is automatic, but
  // the point of the test is that StepSequence never produces an index
  // outside the 2-entry sequence.
}

TEST_CASE(
    "Bug 1 fix (reproduction): starting on MCap USD, Next → MCap EUR → "
    "next kind (matches the task spec's expected ordering)") {
  // screenOrder [BlockHeight, MarketCap, Halving] with 2 currencies.
  const auto seq = rp::BuildRotationSequence("0,30,4", kAllEnabled, 2);
  REQUIRE(seq.size() == 4);
  CHECK(seq[0] == Slot(sm::kApiIdBlockHeight, 2));
  CHECK(seq[1] == Slot(sm::kApiIdMarketCap, 2, 0));
  CHECK(seq[2] == Slot(sm::kApiIdMarketCap, 2, 1));
  CHECK(seq[3] == Slot(sm::kApiIdHalving, 2));

  // Start on MCap USD (idx=1). Step 1 → MCap EUR; step 2 → Halving.
  auto [slot1, idx1] = rp::StepSequence(seq, 1, +1);
  CHECK(slot1 == Slot(sm::kApiIdMarketCap, 2, 1));
  auto [slot2, _] = rp::StepSequence(seq, idx1, +1);
  CHECK(slot2 == Slot(sm::kApiIdHalving, 2));  // never GBP / per-currency
}

TEST_CASE(
    "Bug 2 fix: user screenOrder [0,20,10] with screen 3 disabled and "
    "2 currencies walks BlockHeight → BtcPrice(×2) → MoscowTime(×2) → "
    "wrap") {
  auto enabled = [](int api_id) { return api_id != sm::kApiIdClock; };
  const auto seq = rp::BuildRotationSequence("0,3,20,10", enabled, 2);
  REQUIRE(seq.size() == 5);

  std::vector<std::size_t> visited;
  visited.push_back(seq[0]);
  std::size_t idx = 0;
  for (int i = 0; i < 6; ++i) {  // one full cycle + wrap
    auto [slot, new_idx] = rp::StepSequence(seq, idx, +1);
    visited.push_back(slot);
    idx = new_idx;
  }
  // Expected: BlockHeight, Price USD, Price EUR, Moscow USD,
  // Moscow EUR, wrap back to BlockHeight, then Price USD.
  REQUIRE(visited.size() == 7);
  CHECK(visited[0] == Slot(sm::kApiIdBlockHeight, 2));
  CHECK(visited[1] == Slot(sm::kApiIdBtcPrice, 2, 0));
  CHECK(visited[2] == Slot(sm::kApiIdBtcPrice, 2, 1));
  CHECK(visited[3] == Slot(sm::kApiIdMoscowTime, 2, 0));
  CHECK(visited[4] == Slot(sm::kApiIdMoscowTime, 2, 1));
  CHECK(visited[5] == Slot(sm::kApiIdBlockHeight, 2));  // wrap
  CHECK(visited[6] == Slot(sm::kApiIdBtcPrice, 2, 0));
  // Critically: no Clock slot appears.
  const std::size_t clock = Slot(sm::kApiIdClock, 2);
  for (const auto s : visited) CHECK(s != clock);
}

TEST_CASE("skip predicate hides earnings on solo pool (composed)") {
  // Classic regression: solo-pool earnings hidden via skip predicate.
  // screenOrder includes earnings (71). Predicate skips its slot.
  const auto seq = rp::BuildRotationSequence("0,70,71,4", kAllEnabled, 1);
  REQUIRE(seq.size() == 4);
  CHECK(seq[0] == Slot(sm::kApiIdBlockHeight, 1));
  CHECK(seq[1] == Slot(sm::kApiIdMiningPoolHashrate, 1));
  CHECK(seq[2] == Slot(sm::kApiIdMiningPoolEarnings, 1));
  CHECK(seq[3] == Slot(sm::kApiIdHalving, 1));

  const std::size_t earnings = Slot(sm::kApiIdMiningPoolEarnings, 1);
  auto skip_earnings = [earnings](std::size_t slot) {
    return slot == earnings;  // solo pool hides earnings slot
  };
  // Starting at idx 1 (MiningPoolHashrate), Next should skip earnings
  // and land on Halving.
  auto [slot, idx] = rp::StepSequence(seq, 1, +1, skip_earnings);
  CHECK(slot == Slot(sm::kApiIdHalving, 1));
}

TEST_CASE("actCurrencies=[USD] only: per-currency screens each emit one slot") {
  // The `actCurrencies=[USD]` scenario from the bug report.
  const auto seq = rp::BuildRotationSequence("10,20,30", kAllEnabled, 1);
  REQUIRE(seq.size() == 3);
  CHECK(seq[0] == Slot(sm::kApiIdMoscowTime, 1));
  CHECK(seq[1] == Slot(sm::kApiIdBtcPrice, 1));
  CHECK(seq[2] == Slot(sm::kApiIdMarketCap, 1));

  // Step through: walks Moscow → Price → MCap → Moscow.
  auto [s1, i1] = rp::StepSequence(seq, 0, +1);
  CHECK(s1 == Slot(sm::kApiIdBtcPrice, 1));
  auto [s2, i2] = rp::StepSequence(seq, i1, +1);
  CHECK(s2 == Slot(sm::kApiIdMarketCap, 1));
  auto [s3, _] = rp::StepSequence(seq, i2, +1);
  CHECK(s3 == Slot(sm::kApiIdMoscowTime, 1));
}

TEST_CASE("PrevScreen walks the sequence backwards") {
  const auto seq = rp::BuildRotationSequence("0,20,10", kAllEnabled, 2);
  REQUIRE(seq.size() == 5);
  // From idx 0 (BlockHeight), Prev wraps to the last entry: Moscow EUR.
  auto [slot, idx] = rp::StepSequence(seq, 0, -1);
  CHECK(slot == Slot(sm::kApiIdMoscowTime, 2, 1));
  CHECK(idx == 4);
  // Prev again → Moscow USD.
  auto [slot2, _] = rp::StepSequence(seq, idx, -1);
  CHECK(slot2 == Slot(sm::kApiIdMoscowTime, 2, 0));
}
