// NIP-47 JSON-RPC payload encode/decode helpers.
//
// The wire shape is a JSON-RPCish object inside the encrypted Nostr
// event content. This file builds those objects for outbound
// requests and decodes them for inbound responses + notifications,
// without touching the Nostr envelope or encryption layer.
//
// The encode side is hand-rolled string concatenation — the payloads
// are flat, fixed-shape, and tiny. The decode side uses cJSON because
// the response payloads can be deeply-nested and the wallet may add
// optional fields we don't know about; defensive walking through
// cJSON is cheaper to maintain than a hand-rolled JSON walker that
// only covers v1 fields.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace btclock {
namespace nwc {

// ----- Outbound (kind 23194 plaintext) -----

// Build the plaintext payload for `get_balance`. No params, so the
// output is the literal `{"method":"get_balance","params":{}}`.
std::string BuildGetBalanceRequest();

// Build the plaintext payload for `get_info`. Same shape — no
// params. The wallet's response carries `methods`, `notifications`
// and a few wallet-side details (alias, color, pubkey). We don't
// usually call this on every poll cycle; it's part of the bootstrap
// handshake.
std::string BuildGetInfoRequest();

// ----- Inbound (kind 23195 plaintext) -----

enum class RpcError : uint8_t {
  kOk = 0,
  kNotJson,           // cJSON couldn't parse the bytes
  kMissingResultType, // the `result_type` field is absent or non-string
  kMethodMismatch,    // result_type != expected method
  kWalletError,       // a `code`/`message` was set; details in `wallet_error_*`
  kMissingResult,     // server said success but `result` is missing or wrong shape
};

struct BalanceResponse {
  // From a successful `result.balance` field. Server-side msats. The
  // spec says int64 but we model uint64 — Lightning balances can't go
  // negative through NIP-47 (overdraw shows up as a separate
  // `error.code = INSUFFICIENT_BALANCE` on the next pay_invoice).
  uint64_t balance_msat = 0;
};

struct WalletError {
  std::string code;     // e.g. UNAUTHORIZED, INTERNAL, NOT_IMPLEMENTED
  std::string message;  // human-readable description
};

// Decode the `result_type=get_balance` response. Returns kOk on
// success with `out.balance_msat` populated. If the response is a
// wallet error (`error` set), returns kWalletError with `err`
// populated and `out` left as default-constructed.
RpcError DecodeBalanceResponse(const std::string& json, BalanceResponse& out,
                               WalletError& err);

// Decoded `get_info` response. v1 surfaces just the bits the device
// uses; everything else is silently dropped. `methods` and
// `notifications` are split on whitespace per the wallet-advertised
// space-separated form.
struct InfoResponse {
  std::string alias;
  std::string pubkey;       // wallet's own pubkey (info, not used in tagging)
  std::string network;      // mainnet / testnet / signet / regtest
  uint32_t block_height = 0;
  std::vector<std::string> methods;
  std::vector<std::string> notifications;
};

RpcError DecodeInfoResponse(const std::string& json, InfoResponse& out,
                            WalletError& err);

// ----- INFO event (kind 13194) content + tags decoder -----

// Decode the kind 13194 INFO event. Content is a plaintext
// space-separated method list; tags carry encryption + notification
// type lists. Caller passes the content + already-parsed tags so this
// helper is reusable from host tests without an Event object.
struct InfoEvent {
  std::vector<std::string> methods;
  std::vector<std::string> notifications;
  // Encryption variants advertised by the wallet. Preserved in URI
  // order — caller picks the strongest one we support (nip44_v2 >
  // nip04). Absent encryption tag implies legacy nip04-only.
  std::vector<std::string> encryption;
};

// `tags` is a flat list of [tag-name, value...] subarrays (matching
// the Nostr Event::tags shape). Whitespace splits everything per the
// spec. Returns true iff content was non-empty (a wallet emitting an
// empty INFO would be malformed).
bool DecodeInfoEvent(const std::string& content,
                     const std::vector<std::vector<std::string>>& tags,
                     InfoEvent& out);

// ----- Notification (kind 23197 / 23196 plaintext) -----

enum class PaymentDirection : uint8_t {
  kUnknown = 0,
  kIncoming = 1,  // payment_received
  kOutgoing = 2,  // payment_sent
};

struct PaymentNotification {
  PaymentDirection direction = PaymentDirection::kUnknown;
  // amount + fees are msat per NIP-47 §"payment_received".
  uint64_t amount_msat = 0;
  uint64_t fees_paid_msat = 0;
  // Optional descriptive metadata. Useful for the transient
  // notification screen + WebUI toast (lwf.6).
  std::string description;
  std::string payment_hash;
  // Wall-clock timestamps the wallet attached. settled_at is the most
  // user-meaningful one (when the wallet actually saw the payment).
  // 0 means absent.
  uint64_t created_at = 0;
  uint64_t settled_at = 0;
};

// Decode an NWC notification payload (already decrypted; pass the
// inner JSON string, NOT the encrypted base64 event content). Any
// `notification_type` other than `payment_received` / `payment_sent`
// yields `direction = kUnknown` and is otherwise zero. We still
// return kOk in that case so the caller can log+drop without
// scraping error codes.
RpcError DecodePaymentNotification(const std::string& json,
                                   PaymentNotification& out);

}  // namespace nwc
}  // namespace btclock
