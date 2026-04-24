// Host tests for the NIP-01 envelope parser and NIP-57 zap helpers.
//
// Proves we can pull the amount + bolt11 out of a hand-crafted zap
// receipt without depending on a live relay. These are the two fields
// the ZapListener surfaces to the application.

#include "doctest.h"

#include <cstdint>
#include <string>

#include "data_core/snapshot.hpp"
#include "nostr/event.hpp"
#include "nostr/parser.hpp"
#include "nostr/subscription_manager.hpp"
#include "nostr/zap_listener.hpp"

using namespace btclock::nostr;
using btclock::DataSnapshot;

TEST_CASE("ParseEventObject: flat event with tags and content") {
  const std::string obj = R"({
    "id":"abc",
    "pubkey":"deadbeef",
    "created_at":1745323200,
    "kind":30078,
    "tags":[["d","price:USD"],["source","priceAggregate"],["block","870123"]],
    "content":"64321.50",
    "sig":"00"
  })";

  Event ev;
  REQUIRE(ParseEventObject(obj, ev));
  CHECK(ev.id == "abc");
  CHECK(ev.pubkey == "deadbeef");
  CHECK(ev.created_at == 1745323200u);
  CHECK(ev.kind == 30078u);
  CHECK(ev.content == "64321.50");
  REQUIRE(ev.tags.size() == 3);
  CHECK(ev.TagValue("d") == "price:USD");
  CHECK(ev.TagValue("source") == "priceAggregate");
  CHECK(ev.TagValue("block") == "870123");
}

TEST_CASE("ParseEnvelope: EVENT frame round-trip") {
  const std::string frame = R"(["EVENT","sub1",{
    "id":"ff",
    "pubkey":"aa",
    "created_at":1,
    "kind":30078,
    "tags":[["d","blockheight"]],
    "content":"870124",
    "sig":"00"
  }])";

  Envelope env;
  REQUIRE(ParseEnvelope(frame, env));
  CHECK(env.type == EnvelopeType::kEvent);
  CHECK(env.sub_id == "sub1");
  CHECK(env.event.kind == 30078u);
  CHECK(env.event.content == "870124");
  CHECK(env.event.TagValue("d") == "blockheight");
}

TEST_CASE("ParseEnvelope: EOSE and NOTICE shapes") {
  Envelope env;
  REQUIRE(ParseEnvelope(R"(["EOSE","sub1"])", env));
  CHECK(env.type == EnvelopeType::kEose);
  CHECK(env.sub_id == "sub1");

  REQUIRE(ParseEnvelope(R"(["NOTICE","rate-limited"])", env));
  CHECK(env.type == EnvelopeType::kNotice);
  CHECK(env.message == "rate-limited");
}

// Sample NIP-57 zap receipt. Hand-crafted to match the shape documented
// at https://nips.nostr.com/57 — `amount` tag carries msat as a decimal
// string, `bolt11` tag carries the invoice. Content is the zapper's
// comment (may be empty in the wild).
TEST_CASE("Zap receipt: amount (msat) and bolt11 are extracted") {
  const std::string zap_envelope = R"(["EVENT","zap-sub",{
    "id":"e1a5c...",
    "pubkey":"c0ffee",
    "created_at":1745323222,
    "kind":9735,
    "tags":[
      ["p","recipientpubkeyhexdeadbeef"],
      ["amount","21000"],
      ["bolt11","lnbc210n1pjexample"],
      ["description","{\"kind\":9734}"],
      ["preimage","0011"]
    ],
    "content":"onward and upward!",
    "sig":"00"
  }])";

  Envelope env;
  REQUIRE(ParseEnvelope(zap_envelope, env));
  REQUIRE(env.type == EnvelopeType::kEvent);
  const Event& ev = env.event;
  CHECK(ev.kind == kKindZapReceipt);
  CHECK(ev.content == "onward and upward!");

  uint64_t msat = 0;
  REQUIRE(ExtractZapAmountMsat(ev, msat));
  CHECK(msat == 21000u);

  std::string bolt11;
  REQUIRE(ExtractZapBolt11(ev, bolt11));
  CHECK(bolt11 == "lnbc210n1pjexample");
}

TEST_CASE("Zap receipt: missing amount/bolt11 surfaces as false") {
  const std::string bare = R"({
    "id":"x",
    "pubkey":"y",
    "created_at":1,
    "kind":9735,
    "tags":[["p","abc"]],
    "content":"",
    "sig":"0"
  })";
  Event ev;
  REQUIRE(ParseEventObject(bare, ev));

  uint64_t msat = 99;
  CHECK_FALSE(ExtractZapAmountMsat(ev, msat));
  CHECK(msat == 99);  // unchanged on failure

  std::string bolt11 = "unchanged";
  CHECK_FALSE(ExtractZapBolt11(ev, bolt11));
  CHECK(bolt11 == "unchanged");
}

// --- Bug-1 gate: ShouldSurfaceZap --------------------------------------
// Zero-sat zap receipts (relay-malformed NIP-57 events) should never
// reach the overlay / LED flash / LatestZap snapshot update. The gate
// lives on the parser helper so a host test can pin it without
// standing up the whole SubscriptionManager. The listener calls it
// inline in Handle() — see components/nostr/src/zap_listener.cpp.

TEST_CASE("ShouldSurfaceZap: 0-sat receipt is ignored") {
  const std::string zero = R"({
    "id":"z0",
    "pubkey":"c0ffee",
    "created_at":1,
    "kind":9735,
    "tags":[["p","abc"],["amount","0"],["bolt11","lnbc0"]],
    "content":"",
    "sig":"0"
  })";
  Event ev;
  REQUIRE(ParseEventObject(zero, ev));
  CHECK_FALSE(ShouldSurfaceZap(ev));
}

TEST_CASE("ShouldSurfaceZap: sub-sat (amount<1000 msat) is ignored") {
  // 500 msat == 0.5 sat. NIP-57 allows sub-sat in theory; the firmware
  // rounds to sats for display and 0 sats is the "nothing to show"
  // case we're gating out.
  const std::string sub = R"({
    "id":"zs",
    "pubkey":"c0ffee",
    "created_at":1,
    "kind":9735,
    "tags":[["p","abc"],["amount","500"]],
    "content":"",
    "sig":"0"
  })";
  Event ev;
  REQUIRE(ParseEventObject(sub, ev));
  CHECK_FALSE(ShouldSurfaceZap(ev));
}

TEST_CASE("ShouldSurfaceZap: missing amount tag is ignored") {
  const std::string no_amt = R"({
    "id":"zm",
    "pubkey":"c0ffee",
    "created_at":1,
    "kind":9735,
    "tags":[["p","abc"],["bolt11","lnbc"]],
    "content":"",
    "sig":"0"
  })";
  Event ev;
  REQUIRE(ParseEventObject(no_amt, ev));
  CHECK_FALSE(ShouldSurfaceZap(ev));
}

TEST_CASE("ShouldSurfaceZap: 1-sat receipt surfaces (edge case)") {
  // 1000 msat == 1 sat. Boundary of the gate.
  const std::string one = R"({
    "id":"z1",
    "pubkey":"c0ffee",
    "created_at":1,
    "kind":9735,
    "tags":[["p","abc"],["amount","1000"],["bolt11","lnbc10n"]],
    "content":"",
    "sig":"0"
  })";
  Event ev;
  REQUIRE(ParseEventObject(one, ev));
  CHECK(ShouldSurfaceZap(ev));
}

TEST_CASE("ShouldSurfaceZap: 21k sat receipt surfaces") {
  const std::string k21 = R"({
    "id":"z21k",
    "pubkey":"c0ffee",
    "created_at":1,
    "kind":9735,
    "tags":[["p","abc"],["amount","21000000"],["bolt11","lnbc210u"]],
    "content":"",
    "sig":"0"
  })";
  Event ev;
  REQUIRE(ParseEventObject(k21, ev));
  CHECK(ShouldSurfaceZap(ev));
}

TEST_CASE("ShouldSurfaceZap: non-zap kind never surfaces") {
  // Defensive — a mis-routed frame with a valid amount but wrong kind
  // should not be surfaced even if the listener somehow forwards it.
  const std::string wrong_kind = R"({
    "id":"zk",
    "pubkey":"c0ffee",
    "created_at":1,
    "kind":30078,
    "tags":[["amount","1000"]],
    "content":"",
    "sig":"0"
  })";
  Event ev;
  REQUIRE(ParseEventObject(wrong_kind, ev));
  CHECK_FALSE(ShouldSurfaceZap(ev));
}

TEST_CASE("BuildReqJson: kind + author filter is a valid NIP-01 REQ") {
  Filter f;
  f.kinds.push_back(30078);
  f.authors.push_back("deadbeef");

  const std::string req = BuildReqJson("s1", f);
  CHECK(req == R"(["REQ","s1",{"kinds":[30078],"authors":["deadbeef"]}])");

  // And the parsed-back shape: should at minimum start with ["REQ", ...].
  CHECK(req.rfind("[\"REQ\"", 0) == 0);
}

TEST_CASE("BuildReqJson: zap filter emits #p") {
  Filter f;
  f.kinds.push_back(kKindZapReceipt);
  f.p_tags.push_back("aabbcc");
  const std::string req = BuildReqJson("zap-sub", f);
  CHECK(req == R"(["REQ","zap-sub",{"kinds":[9735],"#p":["aabbcc"]}])");
}

TEST_CASE("BuildReqJson: since + limit emit on the wire in order") {
  // NIP-01 filter fields; `since` scopes the stored-event replay to the
  // 15-minute window we care about, `limit:1` caps that replay at the
  // single most recent event. Both are SHOULDs, so we emit them here
  // and back them up with the arrival-time guard in ZapListener.
  Filter f;
  f.kinds.push_back(kKindZapReceipt);
  f.p_tags.push_back("aabbcc");
  f.since = 1745000000;
  f.limit = 1;
  const std::string req = BuildReqJson("zap-sub", f);
  CHECK(req ==
        R"(["REQ","zap-sub",{"kinds":[9735],"#p":["aabbcc"],)"
        R"("since":1745000000,"limit":1}])");
}

TEST_CASE("BuildReqJson: since alone (no limit) still emits") {
  Filter f;
  f.kinds.push_back(kKindZapReceipt);
  f.since = 42;
  const std::string req = BuildReqJson("s", f);
  CHECK(req == R"(["REQ","s",{"kinds":[9735],"since":42}])");
}

TEST_CASE("BuildCloseJson: matches NIP-01") {
  CHECK(BuildCloseJson("s1") == R"(["CLOSE","s1"])");
}

// --- ShouldShowZap: arrival-time 15-min age + dedupe gate -----------------
// Pins the defensive layer that runs inside ZapListener::Handle. Relays
// SHOULD honour `since` and `limit:1` but NIP-01 allows them not to, so
// we gate on arrival too. See kZapMaxAgeSeconds in zap_listener.hpp.

TEST_CASE("ShouldShowZap: fresh zap (just now) surfaces") {
  constexpr int64_t now = 1'745'000'000;
  CHECK(ShouldShowZap(now, now, /*last=*/0));
  CHECK(ShouldShowZap(now, now - 1, /*last=*/0));
}

TEST_CASE("ShouldShowZap: 14-minute-old zap surfaces") {
  constexpr int64_t now = 1'745'000'000;
  const int64_t fourteen_min_ago = now - (14 * 60);
  CHECK(ShouldShowZap(now, fourteen_min_ago, /*last=*/0));
}

TEST_CASE("ShouldShowZap: exactly at the 15-minute cutoff surfaces") {
  // Documented behaviour: the boundary is inclusive. A zap stamped
  // exactly kZapMaxAgeSeconds ago still shows.
  constexpr int64_t now = 1'745'000'000;
  const int64_t at_cutoff = now - kZapMaxAgeSeconds;
  CHECK(ShouldShowZap(now, at_cutoff, /*last=*/0));
}

TEST_CASE("ShouldShowZap: one second past the cutoff is dropped") {
  constexpr int64_t now = 1'745'000'000;
  const int64_t just_past = now - (kZapMaxAgeSeconds + 1);
  CHECK_FALSE(ShouldShowZap(now, just_past, /*last=*/0));
}

TEST_CASE("ShouldShowZap: 16-minute-old zap is dropped") {
  constexpr int64_t now = 1'745'000'000;
  const int64_t sixteen_min_ago = now - (16 * 60);
  CHECK_FALSE(ShouldShowZap(now, sixteen_min_ago, /*last=*/0));
}

TEST_CASE("ShouldShowZap: event at or before last-shown is dropped") {
  constexpr int64_t now = 1'745'000'000;
  const int64_t last = now - 10;
  // Same timestamp as last-shown → drop (relay re-delivered it).
  CHECK_FALSE(ShouldShowZap(now, last, last));
  // Older than last-shown → drop (out-of-order stored replay).
  CHECK_FALSE(ShouldShowZap(now, last - 5, last));
  // Strictly newer than last-shown AND within window → show.
  CHECK(ShouldShowZap(now, last + 1, last));
}

TEST_CASE("ShouldShowZap: future-dated event (clock skew) surfaces") {
  // Don't drop on NTP jitter. The `since` filter + dedupe already
  // bound this from the other side.
  constexpr int64_t now = 1'745'000'000;
  CHECK(ShouldShowZap(now, now + 5, /*last=*/0));
}

TEST_CASE("ShouldShowZap: dedupe check ignored when last-shown is 0") {
  // First zap of the session: the sentinel value 0 disables dedupe.
  constexpr int64_t now = 1'745'000'000;
  CHECK(ShouldShowZap(now, 1, /*last=*/0) == false);  // too old
  CHECK(ShouldShowZap(now, now - 60, /*last=*/0));    // fresh, no dedupe
}

// --- ParseNip78Content ------------------------------------------------
//
// Covers the d-tag dispatch used by NostrDataSource::OnEvent. Each
// slot per NOSTR.md has its own case; unknown d and bad content both
// surface as false-return so the caller can log+drop without
// corrupting the hub's snapshot.

TEST_CASE("ParseNip78Content: d=blockheight populates block_height") {
  DataSnapshot s;
  REQUIRE(ParseNip78Content("blockheight", "870124", s));
  REQUIRE(s.block_height.has_value());
  CHECK(*s.block_height == 870124u);
  CHECK_FALSE(s.block_fee.has_value());
  CHECK_FALSE(s.block_fee_precise.has_value());
  CHECK(s.prices.empty());
}

TEST_CASE("ParseNip78Content: d=medianFee populates both fee fields") {
  DataSnapshot s;
  REQUIRE(ParseNip78Content("medianFee", "12.75", s));
  REQUIRE(s.block_fee_precise.has_value());
  CHECK(*s.block_fee_precise == doctest::Approx(12.75));
  REQUIRE(s.block_fee.has_value());
  CHECK(*s.block_fee == 13);  // round-half-away-from-zero

  // Exact integer string → integer rounds trivially.
  DataSnapshot s2;
  REQUIRE(ParseNip78Content("medianFee", "12", s2));
  CHECK(*s2.block_fee_precise == doctest::Approx(12.0));
  CHECK(*s2.block_fee == 12);

  // Below-half rounds down.
  DataSnapshot s3;
  REQUIRE(ParseNip78Content("medianFee", "12.4", s3));
  CHECK(*s3.block_fee == 12);
}

TEST_CASE("ParseNip78Content: d=price:<CCY> populates prices map verbatim") {
  DataSnapshot s;
  REQUIRE(ParseNip78Content("price:USD", "64321.50", s));
  REQUIRE(s.prices.count("USD") == 1);
  CHECK(s.prices["USD"] == "64321.50");
  CHECK_FALSE(s.block_height.has_value());

  // Another currency maps to its own slot.
  REQUIRE(ParseNip78Content("price:EUR", "60000.00", s));
  CHECK(s.prices["EUR"] == "60000.00");
  CHECK(s.prices["USD"] == "64321.50");  // unchanged
}

TEST_CASE("ParseNip78Content: unknown d tag returns false, snapshot unchanged") {
  DataSnapshot s;
  s.block_height = 42;  // pre-populated to check non-mutation
  CHECK_FALSE(ParseNip78Content("totallyUnknown", "whatever", s));
  CHECK_FALSE(ParseNip78Content("price:", "64321.50", s));  // empty ccy
  CHECK_FALSE(ParseNip78Content("", "64321.50", s));
  REQUIRE(s.block_height.has_value());
  CHECK(*s.block_height == 42u);
  CHECK(s.prices.empty());
}

TEST_CASE("ParseNip78Content: malformed content returns false") {
  DataSnapshot s;
  // Non-numeric block height.
  CHECK_FALSE(ParseNip78Content("blockheight", "not-a-number", s));
  CHECK_FALSE(ParseNip78Content("blockheight", "", s));
  CHECK_FALSE(ParseNip78Content("blockheight", "123abc", s));

  // Exponent notation rejected (publisher emits plain decimal per NOSTR.md).
  CHECK_FALSE(ParseNip78Content("medianFee", "1e2", s));
  CHECK_FALSE(ParseNip78Content("medianFee", "", s));
  CHECK_FALSE(ParseNip78Content("medianFee", "12.3.4", s));

  // Empty price content is dropped; an empty string would otherwise
  // poison the renderer.
  CHECK_FALSE(ParseNip78Content("price:USD", "", s));

  // Overflow — value > uint32 max. NIP-78 height for bitcoin is safely
  // within 32-bit for the next few centuries; still worth asserting.
  CHECK_FALSE(ParseNip78Content("blockheight", "99999999999", s));
}
