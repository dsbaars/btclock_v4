// Host tests for the NIP-01 envelope parser and NIP-57 zap helpers.
//
// Proves we can pull the amount + bolt11 out of a hand-crafted zap
// receipt without depending on a live relay. These are the two fields
// the ZapListener surfaces to the application.

#include "doctest.h"

#include <cstdint>
#include <string>

#include "nostr/event.hpp"
#include "nostr/parser.hpp"
#include "nostr/subscription_manager.hpp"

using namespace btclock::nostr;

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

TEST_CASE("BuildCloseJson: matches NIP-01") {
  CHECK(BuildCloseJson("s1") == R"(["CLOSE","s1"])");
}
