// Pin the tip extraction from mempool.space's `blocks` snapshot array.
//
// mempool.space sends the last 8 confirmed blocks ASCENDING (oldest
// first) in the initial frame after `{"action":"want","data":["blocks",
// ...]}`. The original implementation read index 0 and a comment claimed
// it was "the first/newest" — so every boot and every WS reconnect
// reported a height up to 7 blocks stale, until the next `block`
// (singular) push corrected it ~10 minutes later.
//
// The array below is a verbatim capture from
// wss://mempool.space/api/v1/ws on 2026-08-02.

#include <cstdint>
#include <string>

#include "cJSON.h"
#include "doctest.h"
#include "sources/mempool_parse.hpp"

namespace {

// Parse a JSON array literal and hand the cJSON node to the helper.
// Returns the helper's bool; writes the tip through `out`.
bool TipOf(const std::string& json, std::uint32_t* out) {
  cJSON* root = cJSON_Parse(json.c_str());
  REQUIRE(root != nullptr);
  const bool ok = btclock::TipHeightFromBlocksArray(root, out);
  cJSON_Delete(root);
  return ok;
}

}  // namespace

TEST_CASE(
    "TipHeightFromBlocksArray: real mempool.space snapshot is ascending") {
  const std::string kSnapshot =
      R"([{"height":960719},{"height":960720},{"height":960721},)"
      R"({"height":960722},{"height":960723},{"height":960724},)"
      R"({"height":960725},{"height":960726}])";
  std::uint32_t tip = 0;
  CHECK(TipOf(kSnapshot, &tip));
  // The bug returned 960719 here — the OLDEST of the eight.
  CHECK(tip == 960726);
}

TEST_CASE(
    "TipHeightFromBlocksArray: order-independent (descending also works)") {
  // We scan for the max rather than taking the last element, so an
  // upstream ordering flip cannot silently reintroduce the stale-tip bug.
  std::uint32_t tip = 0;
  CHECK(TipOf(R"([{"height":960726},{"height":960725},{"height":960724}])",
              &tip));
  CHECK(tip == 960726);
}

TEST_CASE("TipHeightFromBlocksArray: single-entry array") {
  std::uint32_t tip = 0;
  CHECK(TipOf(R"([{"height":870000}])", &tip));
  CHECK(tip == 870000);
}

TEST_CASE("TipHeightFromBlocksArray: rejects empty / malformed input") {
  std::uint32_t tip = 12345;
  // Empty array — nothing to report, `out` must stay untouched.
  CHECK_FALSE(TipOf("[]", &tip));
  CHECK(tip == 12345);
  // Array of non-objects.
  CHECK_FALSE(TipOf(R"([1,2,3])", &tip));
  CHECK(tip == 12345);
  // Objects without a usable `height`.
  CHECK_FALSE(TipOf(R"([{"id":"abc"},{"height":"nope"}])", &tip));
  CHECK(tip == 12345);
  // Not an array at all.
  CHECK_FALSE(TipOf(R"({"height":960726})", &tip));
  CHECK(tip == 12345);
  // Null node / null out-param must not crash.
  CHECK_FALSE(btclock::TipHeightFromBlocksArray(nullptr, &tip));
  CHECK(tip == 12345);
}

TEST_CASE("TipHeightFromBlocksArray: skips unusable entries, keeps the rest") {
  std::uint32_t tip = 0;
  CHECK(
      TipOf(R"([{"height":960719},{"id":"x"},{"height":960726},null])", &tip));
  CHECK(tip == 960726);
}
