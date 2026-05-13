// Host tests for the NWC pairing URI parser.

#include "doctest.h"
#include "nwc/uri.hpp"

using namespace btclock::nwc;

namespace {

// Canonical spec example URI (NIP-47 §"Example connection string").
constexpr const char* kSpecExample =
    "nostr+walletconnect://"
    "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4"
    "?relay=wss%3A%2F%2Frelay.damus.io"
    "&secret="
    "71a8c14c1407c113601079c4302dab36460f0ccd0ad506f1f2dc73b5100e4f3c";

}  // namespace

TEST_CASE("ParsePairingUri: spec example decodes cleanly") {
  PairingUri uri;
  REQUIRE(ParsePairingUri(kSpecExample, uri) == ParseError::kOk);
  CHECK(uri.wallet_pubkey_hex ==
        "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4");
  CHECK(uri.secret_hex ==
        "71a8c14c1407c113601079c4302dab36460f0ccd0ad506f1f2dc73b5100e4f3c");
  REQUIRE(uri.relays.size() == 1);
  CHECK(uri.relays[0] == "wss://relay.damus.io");
}

TEST_CASE("ParsePairingUri: accepts multiple relay= params in URI order") {
  const std::string in =
      "nostr+walletconnect://"
      "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4"
      "?relay=wss%3A%2F%2Falpha.example"
      "&relay=wss%3A%2F%2Fbeta.example"
      "&secret="
      "71a8c14c1407c113601079c4302dab36460f0ccd0ad506f1f2dc73b5100e4f3c";
  PairingUri uri;
  REQUIRE(ParsePairingUri(in, uri) == ParseError::kOk);
  REQUIRE(uri.relays.size() == 2);
  CHECK(uri.relays[0] == "wss://alpha.example");
  CHECK(uri.relays[1] == "wss://beta.example");
}

TEST_CASE("ParsePairingUri: surfaces optional lud16") {
  const std::string in =
      "nostr+walletconnect://"
      "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4"
      "?relay=wss%3A%2F%2Frelay.example"
      "&secret="
      "71a8c14c1407c113601079c4302dab36460f0ccd0ad506f1f2dc73b5100e4f3c"
      "&lud16=satoshi%40example.com";
  PairingUri uri;
  REQUIRE(ParsePairingUri(in, uri) == ParseError::kOk);
  CHECK(uri.lud16 == "satoshi@example.com");
}

TEST_CASE("ParsePairingUri: uppercases hex lowered, unknown params ignored") {
  const std::string in =
      "nostr+walletconnect://"
      "B889FF5B1513B641E2A139F661A661364979C5BEEE91842F8F0EF42AB558E9D4"
      "?relay=wss%3A%2F%2Frelay.example"
      "&secret="
      "71A8C14C1407C113601079C4302DAB36460F0CCD0AD506F1F2DC73B5100E4F3C"
      "&name=Alby+Hub"
      "&app=anything";
  PairingUri uri;
  REQUIRE(ParsePairingUri(in, uri) == ParseError::kOk);
  CHECK(uri.wallet_pubkey_hex ==
        "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4");
  CHECK(uri.secret_hex ==
        "71a8c14c1407c113601079c4302dab36460f0ccd0ad506f1f2dc73b5100e4f3c");
}

TEST_CASE("ParsePairingUri: rejects bad scheme") {
  PairingUri uri;
  CHECK(ParsePairingUri("nostr://abc?relay=wss://x&secret=00", uri) ==
        ParseError::kBadScheme);
  CHECK(ParsePairingUri("https://example.com", uri) == ParseError::kBadScheme);
  CHECK(ParsePairingUri("", uri) == ParseError::kBadScheme);
}

TEST_CASE("ParsePairingUri: rejects short pubkey") {
  PairingUri uri;
  CHECK(
      ParsePairingUri("nostr+walletconnect://abc?relay=wss%3A%2F%2Fx&secret=" +
                          std::string(64, '0'),
                      uri) == ParseError::kBadPubkey);
}

TEST_CASE("ParsePairingUri: rejects non-hex pubkey") {
  std::string bad_pub(64, 'z');
  PairingUri uri;
  CHECK(
      ParsePairingUri("nostr+walletconnect://" + bad_pub +
                          "?relay=wss%3A%2F%2Fx&secret=" + std::string(64, '0'),
                      uri) == ParseError::kBadPubkey);
}

TEST_CASE("ParsePairingUri: rejects missing relay") {
  std::string in =
      "nostr+walletconnect://"
      "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4"
      "?secret=" +
      std::string(64, '0');
  PairingUri uri;
  CHECK(ParsePairingUri(in, uri) == ParseError::kBadRelay);
}

TEST_CASE("ParsePairingUri: rejects http:// as relay (must be ws/wss)") {
  std::string in =
      "nostr+walletconnect://"
      "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4"
      "?relay=http%3A%2F%2Frelay.example"
      "&secret=" +
      std::string(64, '0');
  PairingUri uri;
  CHECK(ParsePairingUri(in, uri) == ParseError::kBadRelay);
}

TEST_CASE("ParsePairingUri: rejects missing secret") {
  std::string in =
      "nostr+walletconnect://"
      "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4"
      "?relay=wss%3A%2F%2Frelay.example";
  PairingUri uri;
  CHECK(ParsePairingUri(in, uri) == ParseError::kBadSecret);
}

TEST_CASE("ParsePairingUri: rejects short secret") {
  std::string in =
      "nostr+walletconnect://"
      "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4"
      "?relay=wss%3A%2F%2Frelay.example&secret=cafe";
  PairingUri uri;
  CHECK(ParsePairingUri(in, uri) == ParseError::kBadSecret);
}

TEST_CASE("ParsePairingUri: rejects truncated percent escape") {
  std::string in =
      "nostr+walletconnect://"
      "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4"
      "?relay=wss%3";  // dangling %3
  PairingUri uri;
  CHECK(ParsePairingUri(in, uri) == ParseError::kBadPercentEscape);
}

TEST_CASE("MaskedUri: never echoes the full secret") {
  PairingUri uri;
  REQUIRE(ParsePairingUri(kSpecExample, uri) == ParseError::kOk);
  const std::string masked = MaskedUri(uri);
  // The full secret MUST NOT appear in the masked string. Just the
  // last 4 characters per the convention.
  CHECK(masked.find(uri.secret_hex) == std::string::npos);
  CHECK(masked.find(uri.secret_hex.substr(uri.secret_hex.size() - 4)) !=
        std::string::npos);
  // First relay shows in cleartext so the user can identify the
  // connection.
  CHECK(masked.find(uri.relays[0]) != std::string::npos);
  // Full pubkey is partially redacted but the 8-char prefix is kept.
  CHECK(masked.find(uri.wallet_pubkey_hex.substr(0, 8)) != std::string::npos);
}
