// Host tests for app/rotation_plan.hpp — pure-logic builder that turns
// the `screenOrder` NVS CSV + `screen<id>Visible` toggles + active
// currency list into the ScreenManager's auto-rotate traversal sequence.
//
// These tests pin the two Rev B bugs (actCurrencies ignored + screenOrder
// ignored) at the building-block layer before the ScreenManager tests
// exercise the composed behaviour.

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

}  // namespace

TEST_CASE("empty screenOrder falls back to full slot_map index order") {
  // 2 currencies → 9 agnostic + 6 per-currency + 1 fee = 16 slots
  // (NWC balance lifted kAgnosticSlots from 8 to 9 in bd
  // btclock_v4-lwf.6).
  const auto seq = rp::BuildRotationSequence("", kAllEnabled, 2);
  REQUIRE(seq.size() == sm::SlotCount(2));
  for (std::size_t i = 0; i < seq.size(); ++i) CHECK(seq[i] == i);
}

TEST_CASE("per-currency kinds expand to one slot per active currency") {
  // Just BtcPrice with 2 currencies → slots 10 and 13 (stride 3, offset 1).
  const auto seq = rp::BuildRotationSequence("20", kAllEnabled, 2);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == 11);
  CHECK(seq[1] == 14);
}

TEST_CASE("Bug 1: screenOrder=20 with 4 currencies emits 4 BtcPrice slots") {
  // Pins that per-currency expansion walks every entry in the currencies
  // list when the user has a single BtcPrice in screenOrder. Protects
  // against a future refactor that accidentally stops at currencies[0].
  const auto seq = rp::BuildRotationSequence("20", kAllEnabled, 4);
  REQUIRE(seq.size() == 4);
  CHECK(seq[0] == 11);  // USD price
  CHECK(seq[1] == 14);  // EUR price
  CHECK(seq[2] == 17);  // GBP price
  CHECK(seq[3] == 20);  // JPY price
}

TEST_CASE(
    "Bug 1 root cause: rotation respects currency_count, never walks GBP "
    "with actCurrencies=[USD,EUR]") {
  // If ScreenManager is constructed with `currencies = ["USD","EUR"]`
  // (2 entries), the BuildRotationSequence cannot emit a slot for GBP —
  // even when screenOrder asks for MarketCap (api_id 30). 2 entries max.
  const auto seq = rp::BuildRotationSequence("30", kAllEnabled, 2);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == 12);  // MarketCap USD
  CHECK(seq[1] == 15);  // MarketCap EUR
  // sm::kAgnosticSlots + 3*2 + 2 = 17 — would be GBP's MarketCap slot
  // with 3 currencies. With currencies=2 this slot doesn't exist.
  for (const auto s : seq) CHECK(s != 17);
}

TEST_CASE("Bug 2: screenOrder drives traversal, not slot_map index order") {
  // User's rotation: BlockHeight (0) → BtcPrice (20) → MoscowTime (10)
  // with 2 currencies. Expected: slot 0 → slots 10, 13 → slots 9, 12.
  const auto seq = rp::BuildRotationSequence("0,20,10", kAllEnabled, 2);
  REQUIRE(seq.size() == 5);
  CHECK(seq[0] == 0);   // block height
  CHECK(seq[1] == 11);  // price USD
  CHECK(seq[2] == 14);  // price EUR
  CHECK(seq[3] == 10);  // moscow USD
  CHECK(seq[4] == 13);  // moscow EUR
}

TEST_CASE(
    "Bug 2: disabled screen is dropped from rotation even when in "
    "screenOrder") {
  // User ordered [BlockHeight, Clock, Halving] but has Clock disabled.
  // Clock is api_id 3.
  auto enabled = [](int api_id) { return api_id != 3; };
  const auto seq = rp::BuildRotationSequence("0,3,4", enabled, 1);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == 0);  // BlockHeight
  CHECK(seq[1] == 2);  // Halving (api_id 4, slot 2)
}

TEST_CASE(
    "Bug 2 task-spec shape: [0, 20, 10] with screen 3 disabled walks "
    "BlockHeight → BtcPrice ×C → MoscowTime ×C") {
  // Exact shape called out in the bug report. Clock (api_id 3) disabled.
  // 4 currencies expands per-currency kinds to 4 entries each.
  auto enabled = [](int api_id) { return api_id != 3; };
  const auto seq = rp::BuildRotationSequence("0,20,10", enabled, 4);
  // 1 (block) + 4 (price) + 4 (moscow) = 9.
  REQUIRE(seq.size() == 9);
  CHECK(seq[0] == 0);
  CHECK(seq[1] == 11);  // price USD
  CHECK(seq[2] == 14);
  CHECK(seq[3] == 17);
  CHECK(seq[4] == 20);
  CHECK(seq[5] == 10);  // moscow USD
  CHECK(seq[6] == 13);
  CHECK(seq[7] == 16);
  CHECK(seq[8] == 19);
  for (const auto s : seq) CHECK(s != 1);
}

TEST_CASE("fee-rate slot appears when the user's screenOrder keeps api_id 6") {
  // Fee rate is a trailing singleton — with 2 currencies its slot is
  // SlotCount(2)-1 = 15.
  const auto seq = rp::BuildRotationSequence("0,6", kAllEnabled, 2);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == 0);
  CHECK(seq[1] == 16);
}

TEST_CASE(
    "sequence never wedges: all-disabled order falls back to a single slot") {
  auto enabled = [](int) { return false; };
  const auto seq = rp::BuildRotationSequence("0,3,4", enabled, 2);
  REQUIRE(seq.size() == 1);
  // Arbitrary choice — documented as "slot 0" in the builder. Asserted
  // here so a future refactor can't silently return an empty vector,
  // which would divide-by-zero in the rotation index math.
  CHECK(seq[0] == 0);
}

TEST_CASE("unknown api_ids in screenOrder are silently skipped") {
  // `9999` isn't a registered api_id — drops from the sequence without
  // failing the whole parse.
  const auto seq = rp::BuildRotationSequence("0,9999,4", kAllEnabled, 1);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == 0);
  CHECK(seq[1] == 2);  // api_id 4 (Halving)
}

TEST_CASE("non-numeric tokens in screenOrder are silently skipped") {
  // Defensive: a stray token (e.g. from a corrupted NVS blob or a future
  // firmware that writes extra metadata) must not wedge the parser.
  const auto seq = rp::BuildRotationSequence("0,abc,,4", kAllEnabled, 1);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == 0);
  CHECK(seq[1] == 2);
}

TEST_CASE(
    "backwards compat: empty screenOrder walks every slot (pre-feature "
    "devices upgrade cleanly)") {
  // With 4 currencies and 9 agnostic + 12 per-ccy + 1 fee = 22 slots.
  // The NWC balance slot lifted kAgnosticSlots from 8 to 9.
  const auto seq = rp::BuildRotationSequence("", kAllEnabled, 4);
  REQUIRE(seq.size() == 23);
  for (std::size_t i = 0; i < 22; ++i) CHECK(seq[i] == i);
}

TEST_CASE(
    "cold-boot fallback honours is_enabled — disabled feature slots "
    "never appear at first boot") {
  // Repro for the "fresh device shows mining-pool / bitaxe screens
  // even with miningPoolStats=false and bitaxeEnabled=false" report.
  // Predicate gates ids 70/71 (mining pool) + 80/81 (bitaxe), as the
  // is_enabled lambdas in init_screen_manager / event_loop now do.
  auto enabled = [](int api_id) {
    if (api_id == 70 || api_id == 71) return false;
    if (api_id == 80 || api_id == 81) return false;
    // api_id 72 (mining-pool estimated earnings) shares the
    // miningPoolStats gate in production, so drop it here for the
    // same cold-boot scenario.
    if (api_id == 72) return false;
    return true;
  };
  const auto seq = rp::BuildRotationSequence("", enabled, 1);
  // 10 agnostic slots minus 5 hidden (4,5,6,7,9) + 3 per-ccy + 1 fee = 9.
  // The NWC balance slot (8) is included by default because the cold-
  // boot predicate doesn't gate it off here (the production predicate
  // checks nwc.enabled — exercised in test_screen_rotation).
  REQUIRE(seq.size() == 9);
  CHECK(seq[0] == 0);   // BlockHeight
  CHECK(seq[1] == 1);   // Clock
  CHECK(seq[2] == 2);   // Halving
  CHECK(seq[3] == 3);   // BitcoinSupply
  CHECK(seq[4] == 8);   // NwcBalance (slot 4..7 dropped)
  CHECK(seq[5] == 10);  // Moscow USD
  CHECK(seq[6] == 11);  // Price USD
  CHECK(seq[7] == 12);  // MarketCap USD
  CHECK(seq[8] == 13);  // BlockFeeRate (trailing slot)
  for (const auto s : seq) {
    CHECK(s != 4);
    CHECK(s != 5);
    CHECK(s != 6);
    CHECK(s != 7);
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
  // (api ids 3, 0) the rotation_sequence is [1, 0], so the first
  // paint must be slot 1 (Time).
  const std::vector<std::size_t> seq = {1, 0};
  const auto r = rp::ResumeSlot(rp::kNoSavedSlot, sm::SlotCount(1), seq);
  CHECK(r == std::optional<std::size_t>{1});
}

TEST_CASE("ResumeSlot: persisted slot inside rotation is restored") {
  // User was on slot 4 (the third sequence entry) when the device
  // rebooted — boot must land back on slot 4, not on sequence[0].
  const std::vector<std::size_t> seq = {0, 1, 4, 2};
  const auto r = rp::ResumeSlot(/*saved_slot=*/4u, sm::SlotCount(1), seq);
  CHECK(r == std::optional<std::size_t>{4});
}

TEST_CASE("ResumeSlot: trailing fee-rate slot resumes even without sequence") {
  // The fee-rate slot lives at slot_count - 1 and is *not* part of
  // rotation_sequence_ unless the user keeps api_id 6 in screenOrder.
  // ResumeSlot still restores it because the user explicitly navigated
  // there before the reboot — dropping them onto sequence[0] would
  // surprise them. SlotCount(1) = 10 + 3 + 1 = 14 → fee slot is 13.
  const std::vector<std::size_t> seq = {0, 1, 2};
  const std::size_t slot_count = sm::SlotCount(1);
  REQUIRE(slot_count == 14);
  const auto r = rp::ResumeSlot(/*saved_slot=*/13u, slot_count, seq);
  CHECK(r == std::optional<std::size_t>{13});
}

TEST_CASE(
    "ResumeSlot: persisted slot dropped from rotation falls back to "
    "sequence[0]") {
  // User was on slot 4 (mining-pool earnings) before they PATCHed it
  // out. After reboot the rotation_sequence no longer contains slot 4
  // — boot must land on the user's first-in-order screen rather than
  // restoring a slot they can no longer reach via Next/Prev.
  const std::vector<std::size_t> seq = {0, 1, 2};
  const auto r = rp::ResumeSlot(/*saved_slot=*/4u, sm::SlotCount(1), seq);
  CHECK(r == std::optional<std::size_t>{0});
}

TEST_CASE(
    "ResumeSlot: persisted slot >= slot_count falls back to sequence[0]") {
  // A previous boot saved slot 15 under a 4-currency layout
  // (slot_count = 9+12+1 = 22). User then dropped to a 1-currency
  // layout (slot_count = 13) — slot 15 no longer exists. Must not
  // address out of bounds; sequence[0] is the safe landing.
  const std::vector<std::size_t> seq = {0, 1, 2};
  const auto r = rp::ResumeSlot(/*saved_slot=*/15u, sm::SlotCount(1), seq);
  CHECK(r == std::optional<std::size_t>{0});
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
  const std::vector<std::size_t> seq = {2, 0, 1};
  const auto r = rp::ResumeSlot(/*saved_slot=*/0u, sm::SlotCount(1), seq);
  CHECK(r == std::optional<std::size_t>{0});
}

TEST_CASE("cold-boot fallback never wedges when every api_id is gated off") {
  // Pathological config: predicate rejects everything. Builder must
  // still emit at least one slot so ScreenManager's index math doesn't
  // divide by zero — same guarantee as the screenOrder path.
  auto enabled = [](int) { return false; };
  const auto seq = rp::BuildRotationSequence("", enabled, 2);
  REQUIRE(seq.size() == 1);
  CHECK(seq[0] == 0);
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
  CHECK(seq[0] == 12);
  CHECK(seq[1] == 15);

  auto [slot1, idx1] = rp::StepSequence(seq, 0, +1);
  CHECK(slot1 == 15);  // MCap EUR
  auto [slot2, idx2] = rp::StepSequence(seq, idx1, +1);
  CHECK(slot2 == 12);  // wraps back to MCap USD, NOT GBP
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
  CHECK(seq[0] == 0);
  CHECK(seq[1] == 12);
  CHECK(seq[2] == 15);
  CHECK(seq[3] == 2);

  // Start on MCap USD (idx=1). Step 1 → MCap EUR; step 2 → Halving.
  auto [slot1, idx1] = rp::StepSequence(seq, 1, +1);
  CHECK(slot1 == 15);
  auto [slot2, _] = rp::StepSequence(seq, idx1, +1);
  CHECK(slot2 == 2);  // Halving — NEVER GBP or any other per-currency.
}

TEST_CASE(
    "Bug 2 fix: user screenOrder [0,20,10] with screen 3 disabled and "
    "2 currencies walks BlockHeight → BtcPrice(×2) → MoscowTime(×2) → "
    "wrap") {
  auto enabled = [](int api_id) { return api_id != 3; };
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
  // Expected: 0 (block), 10 (price USD), 13 (price EUR),
  // 9 (moscow USD), 12 (moscow EUR), back to 0, then 10.
  // Per-currency block starts at 9 after the NwcBalance slot landed
  // (bd btclock_v4-lwf.6 shifted every per-currency slot by +1).
  REQUIRE(visited.size() == 7);
  CHECK(visited[0] == 0);
  CHECK(visited[1] == 11);
  CHECK(visited[2] == 14);
  CHECK(visited[3] == 10);
  CHECK(visited[4] == 13);
  CHECK(visited[5] == 0);  // wrap
  CHECK(visited[6] == 11);
  // Critically: no Clock slot (slot_map slot 1) appears.
  for (const auto s : visited) CHECK(s != 1);
}

TEST_CASE("skip predicate hides earnings on solo pool (composed)") {
  // Classic regression: solo-pool earnings hidden via skip predicate.
  // screenOrder includes earnings (71) at slot 5. Predicate skips it.
  const auto seq = rp::BuildRotationSequence("0,70,71,4", kAllEnabled, 1);
  REQUIRE(seq.size() == 4);
  CHECK(seq[0] == 0);
  CHECK(seq[1] == 4);  // MiningPoolHashrate
  CHECK(seq[2] == 5);  // MiningPoolEarnings
  CHECK(seq[3] == 2);  // Halving

  auto skip_earnings = [](std::size_t slot) {
    return slot == 5;  // solo pool hides earnings slot
  };
  // Starting at idx 1 (MiningPoolHashrate), Next should skip earnings
  // and land on Halving.
  auto [slot, idx] = rp::StepSequence(seq, 1, +1, skip_earnings);
  CHECK(slot == 2);  // Halving — NOT 5 (earnings).
}

TEST_CASE("actCurrencies=[USD] only: per-currency screens each emit one slot") {
  // The `actCurrencies=[USD]` scenario from the bug report.
  const auto seq = rp::BuildRotationSequence("10,20,30", kAllEnabled, 1);
  REQUIRE(seq.size() == 3);
  CHECK(seq[0] == 10);  // Moscow USD
  CHECK(seq[1] == 11);  // Price USD
  CHECK(seq[2] == 12);  // MCap USD

  // Step through: walks 9 → 10 → 11 → 9.
  auto [s1, i1] = rp::StepSequence(seq, 0, +1);
  CHECK(s1 == 11);
  auto [s2, i2] = rp::StepSequence(seq, i1, +1);
  CHECK(s2 == 12);
  auto [s3, _] = rp::StepSequence(seq, i2, +1);
  CHECK(s3 == 10);
}

TEST_CASE("PrevScreen walks the sequence backwards") {
  const auto seq = rp::BuildRotationSequence("0,20,10", kAllEnabled, 2);
  REQUIRE(seq.size() == 5);
  // From idx 0 (BlockHeight), Prev → idx 4 (Moscow EUR = slot 12).
  auto [slot, idx] = rp::StepSequence(seq, 0, -1);
  CHECK(slot == 13);
  CHECK(idx == 4);
  // Prev again → Moscow USD (slot 9).
  auto [slot2, _] = rp::StepSequence(seq, idx, -1);
  CHECK(slot2 == 10);
}
