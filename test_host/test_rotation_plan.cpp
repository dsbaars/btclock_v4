// Host tests for app/rotation_plan.hpp — pure-logic builder that turns
// the `screenOrder` NVS CSV + `screen<id>Visible` toggles + active
// currency list into the ScreenManager's auto-rotate traversal sequence.
//
// These tests pin the two Rev B bugs (actCurrencies ignored + screenOrder
// ignored) at the building-block layer before the ScreenManager tests
// exercise the composed behaviour.

#include "doctest.h"

#include <string>
#include <vector>

#include "app/rotation_plan.hpp"
#include "app/screen_slot_map.hpp"

namespace rp = btclock::rotation_plan;
namespace sm = btclock::slot_map;

namespace {

// Default "everything enabled" predicate — the plan builder still honours
// screenOrder and per-currency expansion without any disabled screens.
const auto kAllEnabled = [](int) { return true; };

}  // namespace

TEST_CASE("empty screenOrder falls back to full slot_map index order") {
  // 2 currencies → 8 agnostic + 6 per-currency + 1 fee = 15 slots.
  const auto seq = rp::BuildRotationSequence("", kAllEnabled, 2);
  REQUIRE(seq.size() == sm::SlotCount(2));
  for (std::size_t i = 0; i < seq.size(); ++i) CHECK(seq[i] == i);
}

TEST_CASE("per-currency kinds expand to one slot per active currency") {
  // Just BtcPrice with 2 currencies → slots 9 and 12 (stride 3, offset 1).
  const auto seq = rp::BuildRotationSequence("20", kAllEnabled, 2);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == 9);
  CHECK(seq[1] == 12);
}

TEST_CASE("Bug 1: screenOrder=20 with 4 currencies emits 4 BtcPrice slots") {
  // Pins that per-currency expansion walks every entry in the currencies
  // list when the user has a single BtcPrice in screenOrder. Protects
  // against a future refactor that accidentally stops at currencies[0].
  const auto seq = rp::BuildRotationSequence("20", kAllEnabled, 4);
  REQUIRE(seq.size() == 4);
  CHECK(seq[0] == 9);   // USD price
  CHECK(seq[1] == 12);  // EUR price
  CHECK(seq[2] == 15);  // GBP price
  CHECK(seq[3] == 18);  // JPY price
}

TEST_CASE(
    "Bug 1 root cause: rotation respects currency_count, never walks GBP "
    "with actCurrencies=[USD,EUR]") {
  // If ScreenManager is constructed with `currencies = ["USD","EUR"]`
  // (2 entries), the BuildRotationSequence cannot emit a slot for GBP —
  // even when screenOrder asks for MarketCap (api_id 30). 2 entries max.
  const auto seq = rp::BuildRotationSequence("30", kAllEnabled, 2);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == 10);  // MarketCap USD
  CHECK(seq[1] == 13);  // MarketCap EUR
  // sm::kAgnosticSlots + 3*2 + 2 = 16 — would be GBP's MarketCap slot
  // with 3 currencies. With currencies=2 this slot doesn't exist.
  for (const auto s : seq) CHECK(s != 16);
}

TEST_CASE("Bug 2: screenOrder drives traversal, not slot_map index order") {
  // User's rotation: BlockHeight (0) → BtcPrice (20) → MoscowTime (10)
  // with 2 currencies. Expected: slot 0 → slots 9, 12 → slots 8, 11.
  const auto seq = rp::BuildRotationSequence("0,20,10", kAllEnabled, 2);
  REQUIRE(seq.size() == 5);
  CHECK(seq[0] == 0);   // block height
  CHECK(seq[1] == 9);   // price USD
  CHECK(seq[2] == 12);  // price EUR
  CHECK(seq[3] == 8);   // moscow USD
  CHECK(seq[4] == 11);  // moscow EUR
}

TEST_CASE(
    "Bug 2: disabled screen is dropped from rotation even when in screenOrder") {
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
  CHECK(seq[1] == 9);
  CHECK(seq[2] == 12);
  CHECK(seq[3] == 15);
  CHECK(seq[4] == 18);
  CHECK(seq[5] == 8);
  CHECK(seq[6] == 11);
  CHECK(seq[7] == 14);
  CHECK(seq[8] == 17);
  // Critically: slot 1 (Clock) never appears.
  for (const auto s : seq) CHECK(s != 1);
}

TEST_CASE("fee-rate slot appears when the user's screenOrder keeps api_id 6") {
  // Fee rate is a trailing singleton — with 2 currencies its slot is
  // SlotCount(2)-1 = 14.
  const auto seq = rp::BuildRotationSequence("0,6", kAllEnabled, 2);
  REQUIRE(seq.size() == 2);
  CHECK(seq[0] == 0);
  CHECK(seq[1] == 14);
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
  // With 4 currencies and 8 agnostic + 12 per-ccy + 1 fee = 21 slots.
  const auto seq = rp::BuildRotationSequence("", kAllEnabled, 4);
  REQUIRE(seq.size() == 21);
  for (std::size_t i = 0; i < 21; ++i) CHECK(seq[i] == i);
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
  CHECK(seq[0] == 10);
  CHECK(seq[1] == 13);

  auto [slot1, idx1] = rp::StepSequence(seq, 0, +1);
  CHECK(slot1 == 13);  // MCap EUR
  auto [slot2, idx2] = rp::StepSequence(seq, idx1, +1);
  CHECK(slot2 == 10);  // wraps back to MCap USD, NOT GBP
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
  CHECK(seq[1] == 10);
  CHECK(seq[2] == 13);
  CHECK(seq[3] == 2);

  // Start on MCap USD (idx=1). Step 1 → MCap EUR; step 2 → Halving.
  auto [slot1, idx1] = rp::StepSequence(seq, 1, +1);
  CHECK(slot1 == 13);
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
  // Expected: 0 (block), 9 (price USD), 12 (price EUR),
  // 8 (moscow USD), 11 (moscow EUR), back to 0, then 9.
  REQUIRE(visited.size() == 7);
  CHECK(visited[0] == 0);
  CHECK(visited[1] == 9);
  CHECK(visited[2] == 12);
  CHECK(visited[3] == 8);
  CHECK(visited[4] == 11);
  CHECK(visited[5] == 0);  // wrap
  CHECK(visited[6] == 9);
  // Critically: no Clock slot (slot_map slot 1) appears.
  for (const auto s : visited) CHECK(s != 1);
}

TEST_CASE("skip predicate hides earnings on solo pool (composed)") {
  // Classic regression: solo-pool earnings hidden via skip predicate.
  // screenOrder includes earnings (71) at slot 5. Predicate skips it.
  const auto seq = rp::BuildRotationSequence("0,70,71,4", kAllEnabled, 1);
  REQUIRE(seq.size() == 4);
  CHECK(seq[0] == 0);
  CHECK(seq[1] == 4);   // MiningPoolHashrate
  CHECK(seq[2] == 5);   // MiningPoolEarnings
  CHECK(seq[3] == 2);   // Halving

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
  CHECK(seq[0] == 8);   // Moscow USD
  CHECK(seq[1] == 9);   // Price USD
  CHECK(seq[2] == 10);  // MCap USD

  // Step through: walks 8 → 9 → 10 → 8.
  auto [s1, i1] = rp::StepSequence(seq, 0, +1);
  CHECK(s1 == 9);
  auto [s2, i2] = rp::StepSequence(seq, i1, +1);
  CHECK(s2 == 10);
  auto [s3, _] = rp::StepSequence(seq, i2, +1);
  CHECK(s3 == 8);
}

TEST_CASE("PrevScreen walks the sequence backwards") {
  const auto seq = rp::BuildRotationSequence("0,20,10", kAllEnabled, 2);
  REQUIRE(seq.size() == 5);
  // From idx 0 (BlockHeight), Prev → idx 4 (Moscow EUR = slot 11).
  auto [slot, idx] = rp::StepSequence(seq, 0, -1);
  CHECK(slot == 11);
  CHECK(idx == 4);
  // Prev again → Moscow USD (slot 8).
  auto [slot2, _] = rp::StepSequence(seq, idx, -1);
  CHECK(slot2 == 8);
}
