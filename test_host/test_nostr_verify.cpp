// Host tests for nostr/event_verify — canonical id recompute + BIP-340
// schnorr signature check.
//
// Fixtures were minted offline with the same vendored libsecp256k1 (a
// throwaway helper at tools/crypto/sign_event.c that signs a hand-built
// canonical-form payload with a fixed seckey). They are real signed
// events, NOT BIP-340 spec vectors — the spec vectors don't carry a
// matching id-recompute case. The seckey is BIP-340 test-vector-1's,
// kept fixed for reproducibility; pubkey + sig regenerate exactly if
// you re-run the helper.

#include <cstring>
#include <string>

#include "doctest.h"
#include "nostr/event.hpp"
#include "nostr/event_verify.hpp"
#include "nostr/parser.hpp"

using namespace btclock::nostr;

namespace {

// Real signed kind 30078 NIP-78 event for d="blockheight", content="870124".
// Pubkey: dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659
constexpr const char* kSignedAppData =
    R"({"id":"b95087a58d5bec716ba3a005f1b5be89f95cd304a53d9bcb926ddc717353894d","pubkey":"dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659","created_at":1714000000,"kind":30078,"tags":[["d","blockheight"]],"content":"870124","sig":"93475a1ca8e579cb6025d9746fd4a6bbbcd34145732f61e77425c23d96c42c13b1f85a8effa4d7f4c30f9e6f89f3a8d82f42db81ef49123806a6b1194a59d943"})";

// Real signed kind 9735 zap receipt fixture (same seckey).
constexpr const char* kSignedZap =
    R"({"id":"9b1c9326ebcbca59c629615686a001b993135208f054f308770a00595eaa301f","pubkey":"dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659","created_at":1714000100,"kind":9735,"tags":[["p","recipient"],["amount","21000"],["bolt11","lnbc210n1pjexample"]],"content":"thanks!","sig":"d13cf3bbae0512e16666a767e842bc149448d1bcc9ef95607e4813850ca790648d873cac934644a2ba941d7aefb0ec9325a927a3a7fd3d07c4330ad1c98470d5"})";

}  // namespace

TEST_CASE("VerifyEvent: real signed kind-30078 event passes") {
  Event ev;
  REQUIRE(ParseEventObject(kSignedAppData, ev));
  CHECK(VerifyEvent(ev) == EventVerifyResult::kOk);
}

TEST_CASE("VerifyEvent: real signed kind-9735 zap event passes") {
  Event ev;
  REQUIRE(ParseEventObject(kSignedZap, ev));
  CHECK(VerifyEvent(ev) == EventVerifyResult::kOk);
}

TEST_CASE("VerifyEvent: tampered content invalidates the recomputed id") {
  Event ev;
  REQUIRE(ParseEventObject(kSignedAppData, ev));
  // Flip a single byte in the content; id no longer matches the
  // canonical hash. Reject before we even get to schnorr.
  ev.content = "870125";
  CHECK(VerifyEvent(ev) == EventVerifyResult::kIdMismatch);
}

TEST_CASE("VerifyEvent: tampered tag invalidates the recomputed id") {
  Event ev;
  REQUIRE(ParseEventObject(kSignedAppData, ev));
  // Add a tag that wasn't in the signed payload.
  Tag t;
  t.values = {"injected", "value"};
  ev.tags.push_back(t);
  CHECK(VerifyEvent(ev) == EventVerifyResult::kIdMismatch);
}

TEST_CASE("VerifyEvent: tampered signature fails schnorr verification") {
  Event ev;
  REQUIRE(ParseEventObject(kSignedAppData, ev));
  // Flip the last hex nibble of the sig — id still matches because we
  // touched the signature, not any id-input field.
  ev.sig.back() = (ev.sig.back() == '0') ? '1' : '0';
  CHECK(VerifyEvent(ev) == EventVerifyResult::kSchnorrInvalid);
}

TEST_CASE("VerifyEvent: tampered id fails before signature check") {
  Event ev;
  REQUIRE(ParseEventObject(kSignedAppData, ev));
  // Flip a hex nibble in the id; the recomputed canonical hash still
  // matches the original id, so this fails the id-vs-claimed check.
  ev.id.back() = (ev.id.back() == '0') ? '1' : '0';
  CHECK(VerifyEvent(ev) == EventVerifyResult::kIdMismatch);
}

TEST_CASE("VerifyEvent: malformed hex lengths fail fast") {
  Event ev;
  REQUIRE(ParseEventObject(kSignedAppData, ev));
  Event short_id = ev;
  short_id.id.pop_back();
  CHECK(VerifyEvent(short_id) == EventVerifyResult::kBadHexLength);

  Event short_pk = ev;
  short_pk.pubkey.pop_back();
  CHECK(VerifyEvent(short_pk) == EventVerifyResult::kBadHexLength);

  Event short_sig = ev;
  short_sig.sig.pop_back();
  CHECK(VerifyEvent(short_sig) == EventVerifyResult::kBadHexLength);

  Event nonhex = ev;
  nonhex.id[0] = 'z';  // not a hex digit
  CHECK(VerifyEvent(nonhex) == EventVerifyResult::kBadHexLength);
}

TEST_CASE("SerializeCanonical: matches the NIP-01 minified array form") {
  Event ev;
  REQUIRE(ParseEventObject(kSignedAppData, ev));
  const std::string canon = SerializeCanonical(ev);
  // Hand-computed reference for the appdata fixture above.
  const std::string expected =
      R"([0,"dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659",1714000000,30078,[["d","blockheight"]],"870124"])";
  CHECK(canon == expected);
}

TEST_CASE("SerializeCanonical: re-escapes content bytes per NIP-01") {
  // Build a synthetic event so we can prove the escape set without
  // having to regenerate fixtures. Quote, backslash, newline, tab,
  // CR, backspace, formfeed all need to come back as escapes.
  Event ev;
  ev.pubkey =
      "0000000000000000000000000000000000000000000000000000000000000000";
  ev.created_at = 1;
  ev.kind = 1;
  ev.content = std::string("a\"b\\c\nd\re\tf\bg\fh");
  const std::string canon = SerializeCanonical(ev);
  // Verify the suffix carries the correctly-escaped content; the
  // surrounding shape is exercised by the appdata fixture above.
  const std::string expected_suffix = R"(,"a\"b\\c\nd\re\tf\bg\fh"])";
  REQUIRE(canon.size() >= expected_suffix.size());
  CHECK(canon.compare(canon.size() - expected_suffix.size(),
                      expected_suffix.size(), expected_suffix) == 0);
}
