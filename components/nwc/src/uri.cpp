// NWC pairing URI parser.
//
// Implementation note: every NWC URI we'll see in production has the
// relay URL percent-encoded (`:` and `/` are reserved per RFC 3986),
// so the decoder is on the hot path. Everything else (lud16, secret,
// pubkey) is either already raw hex or a simple identifier.

#include "nwc/uri.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace btclock {
namespace nwc {
namespace {

constexpr std::string_view kScheme = "nostr+walletconnect://";

bool HexDigitValue(char c, uint8_t& out) {
  if (c >= '0' && c <= '9') {
    out = static_cast<uint8_t>(c - '0');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    out = static_cast<uint8_t>(10 + c - 'a');
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    out = static_cast<uint8_t>(10 + c - 'A');
    return true;
  }
  return false;
}

bool IsHexString(std::string_view s) {
  for (char c : s) {
    uint8_t v;
    if (!HexDigitValue(c, v)) return false;
  }
  return true;
}

std::string LowerHexCopy(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c >= 'A' && c <= 'F') {
      out.push_back(static_cast<char>(c + ('a' - 'A')));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// Percent-decode a URI-encoded chunk. Returns false on truncated /
// non-hex %XX. `+` is NOT decoded as space — NIP-47 URIs are
// %-encoded form-data-style, but the `+` literal is permissible in
// the relay path component and would otherwise corrupt e.g. local
// relays running with paths like `wss://localhost:8080/v2+ndk`.
bool PercentDecode(std::string_view in, std::string& out) {
  out.clear();
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '%') {
      if (i + 2 >= in.size()) return false;
      uint8_t hi, lo;
      if (!HexDigitValue(in[i + 1], hi)) return false;
      if (!HexDigitValue(in[i + 2], lo)) return false;
      out.push_back(static_cast<char>((hi << 4u) | lo));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return true;
}

// Split `kv_block` on `&` into individual `key=value` pairs; for each
// pair `key` is left alone and `value` is percent-decoded into `out`.
// Returns false iff any value has a malformed percent escape.
bool ParseQuery(std::string_view kv_block,
                std::vector<std::pair<std::string, std::string>>& out) {
  out.clear();
  size_t i = 0;
  while (i < kv_block.size()) {
    const size_t amp = kv_block.find('&', i);
    const size_t end = (amp == std::string_view::npos) ? kv_block.size() : amp;
    const auto pair = kv_block.substr(i, end - i);
    const size_t eq = pair.find('=');
    std::string key, value_raw;
    if (eq == std::string_view::npos) {
      key.assign(pair.data(), pair.size());
    } else {
      key.assign(pair.data(), eq);
      value_raw.assign(pair.data() + eq + 1, pair.size() - eq - 1);
    }
    std::string value;
    if (!PercentDecode(value_raw, value)) return false;
    out.emplace_back(std::move(key), std::move(value));
    if (amp == std::string_view::npos) break;
    i = amp + 1;
  }
  return true;
}

bool LooksLikeRelay(std::string_view url) {
  return url.rfind("wss://", 0) == 0 || url.rfind("ws://", 0) == 0;
}

}  // namespace

ParseError ParsePairingUri(const std::string& uri, PairingUri& out) {
  out = PairingUri{};

  if (uri.size() < kScheme.size() ||
      std::string_view(uri).substr(0, kScheme.size()) != kScheme) {
    return ParseError::kBadScheme;
  }

  // After the scheme, the layout is `<pubkey>[?<query>]` with the
  // query parameters comma-style separated by `&`.
  std::string_view rest = std::string_view(uri).substr(kScheme.size());
  std::string_view pub_part;
  std::string_view query_part;
  const size_t qmark = rest.find('?');
  if (qmark == std::string_view::npos) {
    pub_part = rest;
  } else {
    pub_part = rest.substr(0, qmark);
    query_part = rest.substr(qmark + 1);
  }

  if (pub_part.size() != 64 || !IsHexString(pub_part)) {
    return ParseError::kBadPubkey;
  }
  out.wallet_pubkey_hex = LowerHexCopy(pub_part);

  std::vector<std::pair<std::string, std::string>> kvs;
  if (!ParseQuery(query_part, kvs)) return ParseError::kBadPercentEscape;

  for (const auto& [k, v] : kvs) {
    if (k == "relay") {
      if (!LooksLikeRelay(v)) return ParseError::kBadRelay;
      out.relays.push_back(v);
    } else if (k == "secret") {
      if (v.size() != 64 || !IsHexString(v)) return ParseError::kBadSecret;
      out.secret_hex = LowerHexCopy(v);
    } else if (k == "lud16") {
      out.lud16 = v;
    }
    // Unknown keys are silently ignored — future-proof against new
    // NIP-47 params (no spec break expected, but we don't want
    // strict-mode rejection if a wallet adds e.g. `name=...`).
  }

  if (out.relays.empty()) return ParseError::kBadRelay;
  if (out.secret_hex.empty()) return ParseError::kBadSecret;
  return ParseError::kOk;
}

std::string MaskedUri(const PairingUri& parsed) {
  std::string out;
  out.reserve(parsed.wallet_pubkey_hex.size() + 48);
  out.append("nostr+walletconnect://");
  // Prefix 8 chars of the wallet pubkey + ellipsis suffices for the
  // user to recognise which connection this is — operators frequently
  // run several test wallets in parallel.
  if (parsed.wallet_pubkey_hex.size() >= 8) {
    out.append(parsed.wallet_pubkey_hex.substr(0, 8));
    out.append("…");
  } else {
    out.append(parsed.wallet_pubkey_hex);
  }
  out.push_back('?');
  bool first = true;
  for (const auto& r : parsed.relays) {
    out.append(first ? "relay=" : "&relay=");
    out.append(r);
    first = false;
  }
  if (!parsed.secret_hex.empty()) {
    // Full mask — earlier builds leaked the trailing 4 hex chars to
    // help users disambiguate revoked vs current secrets, but the
    // tail is enough entropy to fingerprint a leaked NVS dump in the
    // wild. The presence of the param itself is the only thing the
    // GET caller needs.
    out.append("&secret=…");
  }
  if (!parsed.lud16.empty()) {
    out.append("&lud16=");
    // Show only the first 3 and last 3 chars — enough to recognise
    // which lightning address is configured (e.g. "sat…com" for
    // satoshi@example.com) without exposing the full handle. Very
    // short addresses (< 7 chars) fall back to a full ellipsis since
    // first3/last3 would overlap.
    constexpr std::size_t kHead = 3;
    constexpr std::size_t kTail = 3;
    if (parsed.lud16.size() >= kHead + kTail + 1) {
      out.append(parsed.lud16.substr(0, kHead));
      out.append("…");
      out.append(parsed.lud16.substr(parsed.lud16.size() - kTail));
    } else {
      out.append("…");
    }
  }
  return out;
}

}  // namespace nwc
}  // namespace btclock
