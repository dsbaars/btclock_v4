// NIP-47 JSON-RPC encode/decode helpers.
//
// Decode side uses cJSON because the response shape is open-ended
// (wallets attach a varying mix of optional fields) and a hand-rolled
// walker is too brittle. Encode side is hand-rolled — the request
// payloads are flat and tiny.

#include "nwc/jsonrpc.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"

namespace btclock {
namespace nwc {
namespace {

// Read a numeric field as a non-negative integer. cJSON exposes
// numbers as doubles which loses precision above 2^53 but is fine
// for the int64 msat values NWC actually emits — the entire Bitcoin
// supply at 1 BTC = 1e11 msat fits comfortably below 2^51.
bool ReadUint64(const cJSON* obj, const char* key, uint64_t& out) {
  const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (it == nullptr) return false;
  if (cJSON_IsNumber(it)) {
    if (it->valuedouble < 0.0) return false;
    out = static_cast<uint64_t>(it->valuedouble);
    return true;
  }
  // Some servers stringify large integers to dodge JS double issues;
  // accept those defensively. `int` here means "decimal digits only".
  if (cJSON_IsString(it) && it->valuestring != nullptr) {
    const char* p = it->valuestring;
    if (*p == '\0') return false;
    uint64_t acc = 0;
    while (*p) {
      if (*p < '0' || *p > '9') return false;
      const uint64_t d = static_cast<uint64_t>(*p - '0');
      if (acc > (UINT64_MAX - d) / 10u) return false;
      acc = acc * 10u + d;
      ++p;
    }
    out = acc;
    return true;
  }
  return false;
}

bool ReadString(const cJSON* obj, const char* key, std::string& out) {
  const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (it == nullptr || !cJSON_IsString(it) || it->valuestring == nullptr) {
    return false;
  }
  out.assign(it->valuestring);
  return true;
}

void SplitWhitespace(const std::string& s, std::vector<std::string>& out) {
  out.clear();
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() &&
           (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
      ++i;
    }
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\n' &&
           s[j] != '\r') {
      ++j;
    }
    if (j > i) out.emplace_back(s, i, j - i);
    i = j;
  }
}

bool ExtractWalletError(const cJSON* root, WalletError& err) {
  const cJSON* err_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
  if (err_obj == nullptr || cJSON_IsNull(err_obj)) return false;
  if (!cJSON_IsObject(err_obj)) return false;
  ReadString(err_obj, "code", err.code);
  ReadString(err_obj, "message", err.message);
  return !err.code.empty() || !err.message.empty();
}

// Common preamble for every decode: parse JSON, verify result_type
// matches `expected`. Returns the root on success (caller must
// `cJSON_Delete`) or nullptr on a mismatch with the appropriate
// `err_out` set.
struct DecodeFrame {
  cJSON* root = nullptr;
  RpcError err = RpcError::kOk;
  WalletError wallet_err;
};

DecodeFrame OpenResponse(const std::string& json, const char* expected) {
  DecodeFrame f;
  f.root = cJSON_Parse(json.c_str());
  if (f.root == nullptr) {
    f.err = RpcError::kNotJson;
    return f;
  }
  std::string result_type;
  if (!ReadString(f.root, "result_type", result_type)) {
    f.err = RpcError::kMissingResultType;
    return f;
  }
  if (result_type != expected) {
    f.err = RpcError::kMethodMismatch;
    return f;
  }
  // A wallet-side error is "well-formed but failed" — we still
  // surface kWalletError so the caller can log and back off without
  // having to scrape the JSON itself.
  if (ExtractWalletError(f.root, f.wallet_err)) {
    f.err = RpcError::kWalletError;
    return f;
  }
  return f;
}

}  // namespace

std::string BuildGetBalanceRequest() {
  return std::string(R"({"method":"get_balance","params":{}})");
}

std::string BuildGetInfoRequest() {
  return std::string(R"({"method":"get_info","params":{}})");
}

std::string BuildListTransactionsRequest(int64_t from_secs, int64_t until_secs,
                                         uint32_t limit) {
  // Hand-rolled JSON. The plaintext is signed inside the encrypted
  // event content; cJSON would be heavier than necessary here.
  // `from`/`until`/`limit`/`unpaid` are the spec keys; `unpaid=false`
  // excludes pending invoices so the boot poll only surfaces settled
  // payments. We omit `until` when caller passed 0 (defaults to "now"
  // on the wallet side per NIP-47).
  char buf[192];
  if (until_secs > 0) {
    std::snprintf(
        buf, sizeof(buf),
        "{\"method\":\"list_transactions\",\"params\":{\"from\":%lld,\"until\":"
        "%lld,\"limit\":%u,\"unpaid\":false}}",
        static_cast<long long>(from_secs), static_cast<long long>(until_secs),
        static_cast<unsigned>(limit));
  } else {
    std::snprintf(
        buf, sizeof(buf),
        "{\"method\":\"list_transactions\",\"params\":{\"from\":%lld,\"limit\":"
        "%u,\"unpaid\":false}}",
        static_cast<long long>(from_secs), static_cast<unsigned>(limit));
  }
  return std::string(buf);
}

RpcError DecodeBalanceResponse(const std::string& json, BalanceResponse& out,
                               WalletError& err) {
  DecodeFrame f = OpenResponse(json, "get_balance");
  if (f.err != RpcError::kOk) {
    if (f.err == RpcError::kWalletError) err = f.wallet_err;
    cJSON_Delete(f.root);
    return f.err;
  }
  const cJSON* result = cJSON_GetObjectItemCaseSensitive(f.root, "result");
  if (result == nullptr || !cJSON_IsObject(result)) {
    cJSON_Delete(f.root);
    return RpcError::kMissingResult;
  }
  if (!ReadUint64(result, "balance", out.balance_msat)) {
    cJSON_Delete(f.root);
    return RpcError::kMissingResult;
  }
  cJSON_Delete(f.root);
  return RpcError::kOk;
}

RpcError DecodeInfoResponse(const std::string& json, InfoResponse& out,
                            WalletError& err) {
  DecodeFrame f = OpenResponse(json, "get_info");
  if (f.err != RpcError::kOk) {
    if (f.err == RpcError::kWalletError) err = f.wallet_err;
    cJSON_Delete(f.root);
    return f.err;
  }
  const cJSON* result = cJSON_GetObjectItemCaseSensitive(f.root, "result");
  if (result == nullptr || !cJSON_IsObject(result)) {
    cJSON_Delete(f.root);
    return RpcError::kMissingResult;
  }
  ReadString(result, "alias", out.alias);
  ReadString(result, "pubkey", out.pubkey);
  ReadString(result, "network", out.network);
  uint64_t bh = 0;
  if (ReadUint64(result, "block_height", bh)) {
    out.block_height = static_cast<uint32_t>(bh & 0xffffffffu);
  }
  // methods + notifications can be either a JSON array of strings
  // (this is what `get_info` returns) or a single space-separated
  // string (this is what the kind 13194 INFO event content uses).
  // Handle both shapes here.
  auto extract_list = [&](const char* key, std::vector<std::string>& dest) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(result, key);
    if (it == nullptr) return;
    if (cJSON_IsString(it) && it->valuestring != nullptr) {
      SplitWhitespace(it->valuestring, dest);
    } else if (cJSON_IsArray(it)) {
      dest.clear();
      const cJSON* el = nullptr;
      cJSON_ArrayForEach(el, it) {
        if (cJSON_IsString(el) && el->valuestring != nullptr) {
          dest.emplace_back(el->valuestring);
        }
      }
    }
  };
  extract_list("methods", out.methods);
  extract_list("notifications", out.notifications);
  cJSON_Delete(f.root);
  return RpcError::kOk;
}

bool DecodeInfoEvent(const std::string& content,
                     const std::vector<std::vector<std::string>>& tags,
                     InfoEvent& out) {
  out = InfoEvent{};
  if (content.empty()) return false;
  SplitWhitespace(content, out.methods);
  for (const auto& tag : tags) {
    if (tag.empty()) continue;
    if (tag[0] == "encryption" && tag.size() >= 2) {
      // Each value position past the tag name carries (potentially)
      // its own space-separated list — wallets in the wild use either
      // `["encryption","nip44_v2 nip04"]` or
      // `["encryption","nip44_v2","nip04"]`. Walk every value past
      // index 0 and split it; concatenate the per-value results.
      for (size_t i = 1; i < tag.size(); ++i) {
        std::vector<std::string> chunk;
        SplitWhitespace(tag[i], chunk);
        for (auto& s : chunk) out.encryption.push_back(std::move(s));
      }
    } else if (tag[0] == "notifications" && tag.size() >= 2) {
      for (size_t i = 1; i < tag.size(); ++i) {
        std::vector<std::string> chunk;
        SplitWhitespace(tag[i], chunk);
        for (auto& s : chunk) out.notifications.push_back(std::move(s));
      }
    }
  }
  return true;
}

RpcError DecodeListTransactionsResponse(const std::string& json,
                                        std::vector<PaymentNotification>& out,
                                        WalletError& err) {
  out.clear();
  DecodeFrame f = OpenResponse(json, "list_transactions");
  if (f.err != RpcError::kOk) {
    if (f.err == RpcError::kWalletError) err = f.wallet_err;
    cJSON_Delete(f.root);
    return f.err;
  }
  const cJSON* result = cJSON_GetObjectItemCaseSensitive(f.root, "result");
  if (result == nullptr || !cJSON_IsObject(result)) {
    cJSON_Delete(f.root);
    return RpcError::kMissingResult;
  }
  const cJSON* txs = cJSON_GetObjectItemCaseSensitive(result, "transactions");
  if (txs == nullptr || !cJSON_IsArray(txs)) {
    // An empty/absent transactions array is a valid "no payments in
    // the window" reply — keep the result kOk so the boot-poll path
    // just exits quietly.
    cJSON_Delete(f.root);
    return RpcError::kOk;
  }
  const cJSON* tx = nullptr;
  cJSON_ArrayForEach(tx, txs) {
    if (!cJSON_IsObject(tx)) continue;
    PaymentNotification pn;
    std::string ttype;
    ReadString(tx, "type", ttype);
    if (ttype == "incoming") {
      pn.direction = PaymentDirection::kIncoming;
    } else if (ttype == "outgoing") {
      pn.direction = PaymentDirection::kOutgoing;
    }
    ReadUint64(tx, "amount", pn.amount_msat);
    ReadUint64(tx, "fees_paid", pn.fees_paid_msat);
    ReadString(tx, "description", pn.description);
    ReadString(tx, "payment_hash", pn.payment_hash);
    ReadUint64(tx, "created_at", pn.created_at);
    ReadUint64(tx, "settled_at", pn.settled_at);
    out.push_back(std::move(pn));
  }
  cJSON_Delete(f.root);
  return RpcError::kOk;
}

RpcError DecodePaymentNotification(const std::string& json,
                                   PaymentNotification& out) {
  out = PaymentNotification{};
  cJSON* root = cJSON_Parse(json.c_str());
  if (root == nullptr) return RpcError::kNotJson;

  std::string ntype;
  ReadString(root, "notification_type", ntype);
  if (ntype == "payment_received") {
    out.direction = PaymentDirection::kIncoming;
  } else if (ntype == "payment_sent") {
    out.direction = PaymentDirection::kOutgoing;
  }
  // Unknown notification types still parse the body in case the
  // wallet emits informational data we want to surface later.

  const cJSON* notif = cJSON_GetObjectItemCaseSensitive(root, "notification");
  if (notif != nullptr && cJSON_IsObject(notif)) {
    ReadUint64(notif, "amount", out.amount_msat);
    ReadUint64(notif, "fees_paid", out.fees_paid_msat);
    ReadString(notif, "description", out.description);
    ReadString(notif, "payment_hash", out.payment_hash);
    ReadUint64(notif, "created_at", out.created_at);
    ReadUint64(notif, "settled_at", out.settled_at);
  }
  cJSON_Delete(root);
  return RpcError::kOk;
}

}  // namespace nwc
}  // namespace btclock
