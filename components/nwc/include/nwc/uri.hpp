// Nostr Wallet Connect (NIP-47) pairing URI parser.
//
// The wallet service hands the user a string of the form
//
//   nostr+walletconnect://<wallet-pubkey-hex64>?
//       relay=wss%3A%2F%2Frelay.example.com&
//       secret=<client-seckey-hex64>[&
//       relay=wss%3A%2F%2Fsecond.example.com][&lud16=user%40domain]
//
// Multiple `relay=` parameters are explicitly permitted by the spec
// (NIP-47 §"Nostr Wallet Connect URI"). The parser accepts them in
// the order they appear; the client will pick the first reachable one.
//
// Pure logic, no ESP-IDF; the URI is short enough to walk by hand
// without pulling in a URL parser. Percent-decoding is inlined per
// RFC 3986 §2.1 — every NWC URI in the wild encodes `:` and `/` in
// the relay URL, so the decoder runs on essentially every input.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace btclock {
namespace nwc {

enum class ParseError : uint8_t {
  kOk = 0,
  kBadScheme,         // not `nostr+walletconnect://`
  kBadPubkey,         // missing, wrong length, or non-hex
  kBadSecret,         // missing / wrong length / non-hex
  kBadRelay,          // missing or no wss://|ws:// prefix
  kBadPercentEscape,  // %XX with non-hex digits
};

struct PairingUri {
  // 32-byte wallet-service x-only pubkey, lowercased hex (64 chars).
  std::string wallet_pubkey_hex;
  // 32-byte client secret, lowercased hex (64 chars). This is the
  // private key the client uses both for signing kind 23194 events
  // and as the seckey input to NIP-04 / NIP-44 v2 encryption.
  std::string secret_hex;
  // Relay URLs in URI order. May be empty if the URI was malformed;
  // ParseError will indicate that.
  std::vector<std::string> relays;
  // Optional lightning address (lud16). Empty if absent. Not used
  // by NWC itself — purely informational, surfaced for the UI.
  std::string lud16;
};

// Decode the entire URI. On success returns kOk and populates `out`;
// otherwise returns the first failure mode encountered (parsing
// stops as soon as a required field can't be filled). Validation is
// strict: pubkey + secret MUST be exactly 64 lowercase-able hex
// characters; relay URLs MUST start with `wss://` or `ws://`.
ParseError ParsePairingUri(const std::string& uri, PairingUri& out);

// Best-effort masking for log + WebUI display: keeps the wallet
// pubkey prefix and the first relay, and replaces the secret with
// `…<last 4>` so the URI is recognisable but no longer usable.
// Never logs the full secret.
std::string MaskedUri(const PairingUri& parsed);

}  // namespace nwc
}  // namespace btclock
