// NWC client state machine implementation.
//
// The on-device flow:
//
//   1. Construct an NwcClient with the parsed URI, a sub-manager
//      attached to a dedicated relay (the URL came from the URI), a
//      publish hook (RelayClient::SendText), random/now functors.
//   2. Start() registers a subscription on kinds {13194 INFO, 23195
//      response, 23197 notif modern, 23196 notif legacy} with
//      `#p = our_pubkey`. Filter built from the manager's existing
//      Filter struct — no extra REQ machinery.
//   3. When the relay flushes the stored INFO event (or a fresh one
//      arrives), `HandleEvent` routes it to `OnInfoEvent` which
//      locks in the encryption variant + transitions kBootstrapping
//      → kReady + fires `on_ready_`.
//   4. The owner schedules periodic `RequestGetBalance()` calls;
//      each builds a NIP-47 plaintext, encrypts via the negotiated
//      variant, wraps in a signed kind 23194 event, and publishes.
//   5. The matching kind 23195 response runs through
//      `OnResponse` → decrypt → JSON-decode → `on_balance_`.
//   6. Asynchronous kind 23197 (or 23196) notifications run
//      through `OnNotification` → decrypt → JSON-decode → `on_payment_`.

#include "nwc/client.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "nostr/event_sign.hpp"
#include "nostr/subscription_manager.hpp"

namespace btclock {
namespace nwc {
namespace {

// NIP-47 event kinds.
constexpr uint32_t kKindInfo = 13194;
constexpr uint32_t kKindRequest = 23194;
constexpr uint32_t kKindResponse = 23195;
constexpr uint32_t kKindNotifModern = 23197;  // NIP-44 v2 path
constexpr uint32_t kKindNotifLegacy = 23196;  // NIP-04 fallback

const char* EncryptionTagText(nostr::EncryptionVariant v) {
  return v == nostr::EncryptionVariant::kNip44V2 ? "nip44_v2" : "nip04";
}

// Append a JSON string literal with the minimal NIP-01 escape set.
// Same shape as `event_verify.cpp::AppendJsonString` — we don't reach
// into that file because it's in an anonymous namespace, and the
// payloads here are small enough that a local copy is cheaper than
// extracting a header.
void AppendJsonString(std::string& out, const std::string& s) {
  out.push_back('"');
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      default:
        out.push_back(static_cast<char>(c));
        break;
    }
  }
  out.push_back('"');
}

void AppendUint(std::string& out, uint64_t n) {
  char buf[24];
  size_t i = 0;
  if (n == 0) {
    buf[i++] = '0';
  } else {
    char tmp[24];
    size_t j = 0;
    while (n != 0) {
      tmp[j++] = static_cast<char>('0' + (n % 10));
      n /= 10;
    }
    while (j != 0) buf[i++] = tmp[--j];
  }
  out.append(buf, i);
}

// Serialize a NIP-01 event to the relay wire format:
//   ["EVENT",{"id":"…","pubkey":"…","created_at":…,"kind":…,
//             "tags":[…],"content":"…","sig":"…"}]
// All field bytes are already either plain hex (id/pubkey/sig) or
// the encrypted base64 payload (content) so the only escapes that can
// matter are inside `tags[*][n]` — that uses the same minimal set.
std::string SerializeEventFrame(const nostr::Event& ev) {
  std::string out;
  out.reserve(256 + ev.content.size());
  out.append(R"(["EVENT",{"id":)");
  AppendJsonString(out, ev.id);
  out.append(R"(,"pubkey":)");
  AppendJsonString(out, ev.pubkey);
  out.append(R"(,"created_at":)");
  AppendUint(out, ev.created_at);
  out.append(R"(,"kind":)");
  AppendUint(out, ev.kind);
  out.append(R"(,"tags":[)");
  for (size_t i = 0; i < ev.tags.size(); ++i) {
    if (i != 0) out.push_back(',');
    out.push_back('[');
    const auto& tag = ev.tags[i];
    for (size_t j = 0; j < tag.values.size(); ++j) {
      if (j != 0) out.push_back(',');
      AppendJsonString(out, tag.values[j]);
    }
    out.push_back(']');
  }
  out.append(R"(],"content":)");
  AppendJsonString(out, ev.content);
  out.append(R"(,"sig":)");
  AppendJsonString(out, ev.sig);
  out.append("}]");
  return out;
}

}  // namespace

bool DecodeHex32(const std::string& hex, uint8_t out[32]) {
  if (hex.size() != 64) return false;
  auto nib = [](char c, uint8_t& v) -> bool {
    if (c >= '0' && c <= '9') {
      v = static_cast<uint8_t>(c - '0');
      return true;
    }
    if (c >= 'a' && c <= 'f') {
      v = static_cast<uint8_t>(10 + c - 'a');
      return true;
    }
    if (c >= 'A' && c <= 'F') {
      v = static_cast<uint8_t>(10 + c - 'A');
      return true;
    }
    return false;
  };
  for (size_t i = 0; i < 32; ++i) {
    uint8_t hi, lo;
    if (!nib(hex[2 * i], hi)) return false;
    if (!nib(hex[2 * i + 1], lo)) return false;
    out[i] = static_cast<uint8_t>((hi << 4u) | lo);
  }
  return true;
}

NwcClient::NwcClient(PairingUri pairing, PublishFn publish,
                     SubscribeFn subscribe, UnsubscribeFn unsubscribe)
    : pairing_(std::move(pairing)),
      publish_(std::move(publish)),
      subscribe_(std::move(subscribe)),
      unsubscribe_(std::move(unsubscribe)) {
  // Stable sub-ids keyed on the wallet pubkey prefix — short enough to
  // fit in a single relay line, distinctive enough to survive multi-
  // wallet hosts. Separate ids on the same WSS so the two filter
  // shapes (authors-only INFO vs `#p`-filtered RPC) stay queryable
  // independently.
  const std::string prefix = pairing_.wallet_pubkey_hex.substr(0, 8);
  sub_id_info_ = "nwci-" + prefix;
  sub_id_rpc_ = "nwcr-" + prefix;
}

NwcClient::~NwcClient() = default;

bool NwcClient::LoadKeys() {
  if (keys_loaded_) return true;
  if (!DecodeHex32(pairing_.secret_hex, seckey_)) return false;
  if (!DecodeHex32(pairing_.wallet_pubkey_hex, wallet_pub_)) return false;
  keys_loaded_ = true;
  return true;
}

bool NwcClient::EnsureConversationKey() {
  if (conv_key_loaded_) return true;
  if (!keys_loaded_ && !LoadKeys()) return false;
  if (!nostr::Nip44ConversationKey(seckey_, wallet_pub_, conversation_key_)) {
    return false;
  }
  conv_key_loaded_ = true;
  return true;
}

void NwcClient::Start() {
  if (!LoadKeys()) {
    state_ = State::kFatal;
    return;
  }
  // Eager NIP-44 conversation-key derivation so the notification
  // worker doesn't race with first-decrypt setup. The math is
  // deterministic given the URI keys; failure here means malformed
  // keys, which we'd hit anyway on first request.
  (void)EnsureConversationKey();

  // Derive our x-only pubkey from the secret. Needed for the `#p`
  // filter and as the canonical pubkey field on every kind 23194 we
  // sign.
  uint8_t our_x[32];
  if (nostr::DerivePubkeyXOnly(seckey_, our_x) != nostr::EventSignError::kOk) {
    state_ = State::kFatal;
    return;
  }
  std::string our_pubkey_hex(64, '\0');
  {
    static constexpr char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
      our_pubkey_hex[2 * i] = kHex[(our_x[i] >> 4) & 0xfu];
      our_pubkey_hex[2 * i + 1] = kHex[our_x[i] & 0xfu];
    }
  }

  // Two filters on the same socket. INFO (kind 13194) is a NIP-33
  // replaceable event authored by the wallet service and carries NO
  // `p` tag — folding it into the same filter as the RPC kinds with
  // `#p=[us]` makes the relay drop it. `limit:1` tells the relay to
  // replay just the most recent stored INFO on subscribe.
  nostr::Filter f_info;
  f_info.kinds = {kKindInfo};
  f_info.authors = {pairing_.wallet_pubkey_hex};
  f_info.limit = 1;
  // Responses + notifications. Wallet-signed (so `authors=[wallet]`
  // pins us to the right author), tagged with our pubkey via `p`.
  nostr::Filter f_rpc;
  f_rpc.kinds = {kKindResponse, kKindNotifModern, kKindNotifLegacy};
  f_rpc.authors = {pairing_.wallet_pubkey_hex};
  f_rpc.p_tags = {our_pubkey_hex};
  f_rpc.limit = 0;

  if (subscribe_) {
    subscribe_(sub_id_info_, f_info);
    subscribe_(sub_id_rpc_, f_rpc);
  }
  state_ = State::kBootstrapping;
}

void NwcClient::Stop() {
  if (unsubscribe_) {
    unsubscribe_(sub_id_info_);
    unsubscribe_(sub_id_rpc_);
  }
  state_ = State::kIdle;
  inflight_request_id_.clear();
  inflight_method_.clear();
}

bool NwcClient::PublishSignedRequest(const std::string& plaintext_payload) {
  if (state_ == State::kFatal) return false;
  if (!LoadKeys()) return false;

  // Encrypt the payload. NIP-44 v2 uses a 32-byte nonce; NIP-04 uses
  // a 16-byte IV. Caller-supplied RNG; we pull whichever size we
  // need.
  const bool nip44 = (encryption_ == nostr::EncryptionVariant::kNip44V2);
  uint8_t nonce_buf[32]{};
  const size_t nonce_len = nip44 ? 32u : 16u;
  if (random_) random_(nonce_buf, nonce_len);

  std::string ciphertext_content;
  if (nip44) {
    if (!EnsureConversationKey()) return false;
    ciphertext_content =
        nostr::Nip44EncryptV2(conversation_key_, nonce_buf, plaintext_payload);
  } else {
    ciphertext_content =
        nostr::Nip04Encrypt(seckey_, wallet_pub_, nonce_buf, plaintext_payload);
  }
  if (ciphertext_content.empty()) return false;

  // Build the event object: tags + content + meta. id/pubkey/sig get
  // filled by SignEvent (which also rewrites the pubkey field — we
  // leave it blank here).
  nostr::Event ev;
  ev.created_at = static_cast<uint64_t>(now_ ? now_() : 0);
  ev.kind = kKindRequest;
  ev.content = std::move(ciphertext_content);
  {
    nostr::Tag p_tag;
    p_tag.values = {"p", pairing_.wallet_pubkey_hex};
    ev.tags.push_back(std::move(p_tag));
  }
  {
    nostr::Tag enc_tag;
    enc_tag.values = {"encryption", EncryptionTagText(encryption_)};
    ev.tags.push_back(std::move(enc_tag));
  }

  uint8_t aux[32]{};
  if (random_) random_(aux, sizeof(aux));
  if (nostr::SignEvent(seckey_, aux, ev) != nostr::EventSignError::kOk) {
    return false;
  }
  inflight_request_id_ = ev.id;

  const std::string frame = SerializeEventFrame(ev);
  return publish_ ? publish_(frame.data(), frame.size()) : false;
}

bool NwcClient::RequestGetBalance() {
  inflight_method_ = "get_balance";
  return PublishSignedRequest(BuildGetBalanceRequest());
}

bool NwcClient::RequestGetInfo() {
  inflight_method_ = "get_info";
  return PublishSignedRequest(BuildGetInfoRequest());
}

bool NwcClient::RequestListTransactions(int64_t from_secs, uint32_t limit) {
  inflight_method_ = "list_transactions";
  // until=0 → omit field, wallet defaults to "now". The boot-poll
  // caller always wants now as the upper bound.
  return PublishSignedRequest(
      BuildListTransactionsRequest(from_secs, /*until_secs=*/0, limit));
}

void NwcClient::HandleEvent(const nostr::Event& ev) {
  if (state_ == State::kFatal) return;
  // Count every event the relay handed us BEFORE the per-kind switch
  // so /api/nwc/debug can distinguish "relay never delivered" from
  // "delivered but the per-kind handler dropped it". `now_` may be
  // null pre-Start so guard the timestamp.
  events_total_.fetch_add(1);
  last_kind_.store(ev.kind);
  last_event_ms_.store(now_ ? now_() * 1000 : 0);
  switch (ev.kind) {
    case kKindInfo:
      events_info_.fetch_add(1);
      OnInfoEvent(ev);
      return;
    case kKindResponse:
      events_response_.fetch_add(1);
      OnResponse(ev);
      return;
    case kKindNotifModern:
      events_notif_modern_.fetch_add(1);
      OnNotification(ev);
      return;
    case kKindNotifLegacy:
      events_notif_legacy_.fetch_add(1);
      OnNotification(ev);
      return;
    default:
      // Quietly ignore unrelated kinds — the subscription filter is
      // already kind-restricted but a permissive relay might leak.
      events_other_.fetch_add(1);
      return;
  }
}

void NwcClient::OnInfoEvent(const nostr::Event& ev) {
  // Translate Event::tags to the flat vector<vector<string>> the
  // decoder expects.
  std::vector<std::vector<std::string>> tags_flat;
  tags_flat.reserve(ev.tags.size());
  for (const auto& t : ev.tags) tags_flat.push_back(t.values);

  InfoEvent info;
  if (!DecodeInfoEvent(ev.content, tags_flat, info)) return;

  // Pick the strongest encryption the wallet advertises. Spec: absence
  // of an `encryption` tag implies legacy nip04-only.
  nostr::EncryptionVariant chosen = nostr::EncryptionVariant::kNip04;
  for (const auto& code : info.encryption) {
    if (code == "nip44_v2") {
      chosen = nostr::EncryptionVariant::kNip44V2;
      break;
    }
  }
  encryption_ = chosen;
  state_ = State::kReady;
  if (on_ready_) on_ready_(info);
}

void NwcClient::OnResponse(const nostr::Event& ev) {
  // Re-derive the encryption variant from the event's own
  // `encryption` tag if present. Wallets that downgrade on a
  // per-request basis are rare but legal.
  nostr::EncryptionVariant variant = encryption_;
  for (const auto& tag : ev.tags) {
    if (tag.values.size() >= 2 && tag.values[0] == "encryption") {
      variant = nostr::ParseEncryptionTag(tag.values[1]);
      break;
    }
  }

  if (!LoadKeys()) return;
  // For NIP-44 v2 we can leverage the cached conversation key.
  // Construct a `WithKeys` decrypt either way — keeps the call site
  // simple.
  decrypt_attempts_.fetch_add(1);
  nostr::Nip4xDecryptResult dec =
      nostr::Decrypt(variant, seckey_, wallet_pub_, ev.content);
  if (!dec.ok) {
    if (variant == nostr::EncryptionVariant::kNip44V2) {
      decrypt_fail_nip44_.fetch_add(1);
    } else {
      decrypt_fail_nip04_.fetch_add(1);
    }
    return;
  }
  decrypt_ok_.fetch_add(1);
  last_response_ms_.store(now_ ? now_() * 1000 : 0);

  // Match by e-tag if present; otherwise just trust the kind 23195
  // came in for us (the relay filter already enforces `#p =
  // our_pubkey`).
  bool matches_inflight = false;
  for (const auto& tag : ev.tags) {
    if (tag.values.size() >= 2 && tag.values[0] == "e") {
      matches_inflight = (tag.values[1] == inflight_request_id_);
      break;
    }
  }

  if (inflight_method_ == "get_balance" &&
      (matches_inflight || inflight_request_id_.empty())) {
    BalanceResponse resp;
    WalletError werr;
    const bool decoded =
        DecodeBalanceResponse(dec.plaintext, resp, werr) == RpcError::kOk;
    if (decoded) {
      decode_resp_ok_.fetch_add(1);
      balance_msat_cache_.store(resp.balance_msat);
    } else {
      decode_resp_fail_.fetch_add(1);
    }
    // Clear the inflight bookkeeping BEFORE firing the callback —
    // callbacks may issue a follow-up request (e.g. boot-time
    // list_transactions chained off on_balance) which would set new
    // inflight state; clearing after the callback would nuke it and
    // drop the next response on the floor. We don't auto-retry on
    // kWalletError here — the higher-layer refresh tick will fire
    // the next request.
    inflight_request_id_.clear();
    inflight_method_.clear();
    if (decoded && on_balance_) {
      cb_on_balance_dispatched_.fetch_add(1);
      on_balance_(resp.balance_msat);
    }
    return;
  }
  if (inflight_method_ == "get_info" &&
      (matches_inflight || inflight_request_id_.empty())) {
    InfoResponse info;
    WalletError werr;
    const bool decoded =
        DecodeInfoResponse(dec.plaintext, info, werr) == RpcError::kOk;
    if (decoded) {
      decode_resp_ok_.fetch_add(1);
    } else {
      decode_resp_fail_.fetch_add(1);
    }
    inflight_request_id_.clear();
    inflight_method_.clear();
    if (decoded && on_ready_) {
      InfoEvent legacy_info;
      legacy_info.methods = info.methods;
      legacy_info.notifications = info.notifications;
      on_ready_(legacy_info);
    }
    return;
  }
  if (inflight_method_ == "list_transactions" &&
      (matches_inflight || inflight_request_id_.empty())) {
    std::vector<PaymentNotification> txs;
    WalletError werr;
    const bool decoded = DecodeListTransactionsResponse(dec.plaintext, txs,
                                                        werr) == RpcError::kOk;
    if (decoded) {
      decode_resp_ok_.fetch_add(1);
    } else {
      decode_resp_fail_.fetch_add(1);
    }
    inflight_request_id_.clear();
    inflight_method_.clear();
    if (decoded) {
      // Fan each settled transaction out as if a live notification
      // had landed. Same counters, same on_payment_ callback, same
      // overlay-trigger plumbing — only the most recent one ends up
      // displayed because the screen-manager hand-off is a single
      // pending flag, but every tx still updates the snapshot's
      // `nwc_last_payment` (the *last* iteration wins). Iterate in
      // ascending settled order so "last" = newest.
      std::sort(txs.begin(), txs.end(),
                [](const PaymentNotification& a, const PaymentNotification& b) {
                  return a.settled_at < b.settled_at;
                });
      for (const auto& pn : txs) {
        // Balance bumps stay coherent with the live-notification
        // path so the cache reflects the wallet state. Optimistic
        // arithmetic, same as DispatchHeavy.
        if (pn.direction == PaymentDirection::kIncoming) {
          balance_msat_cache_.fetch_add(pn.amount_msat);
        } else if (pn.direction == PaymentDirection::kOutgoing) {
          const uint64_t total = pn.amount_msat + pn.fees_paid_msat;
          uint64_t cur = balance_msat_cache_.load();
          uint64_t next = (cur > total) ? (cur - total) : 0ULL;
          balance_msat_cache_.store(next);
        }
        last_pay_direction_.store(static_cast<uint8_t>(pn.direction));
        last_pay_amount_sats_.store(
            static_cast<int64_t>(pn.amount_msat / 1000ULL));
        last_pay_received_ms_.store(now_ ? now_() * 1000 : 0);
        if (on_payment_) {
          cb_on_payment_dispatched_.fetch_add(1);
          on_payment_(pn);
        }
      }
    }
    return;
  }
}

void NwcClient::OnNotification(const nostr::Event& ev) {
  // Light path: if an enqueue functor is wired (production), copy the
  // minimal envelope and hand off to the worker. The WS RX-callback
  // task has only ~3-4 KiB of stack; decrypting NIP-44 v2 + cJSON-
  // parsing the payload here overflows it and reboots the device. See
  // bd btclock_v4-lwf.9 for the crash signature.
  if (notif_enqueue_) {
    RawNotification raw;
    raw.kind = ev.kind;
    raw.content = ev.content;
    raw.event_id = ev.id;
    if (notif_enqueue_(std::move(raw))) {
      notif_enqueued_.fetch_add(1);
    } else {
      notif_dropped_.fetch_add(1);
    }
    return;
  }
  // Fallback — no queue wired (host tests, or pre-init). Run the heavy
  // path inline. Behaviour identical to pre-queue NwcClient.
  DispatchHeavy(ev.kind, ev.content);
}

void NwcClient::DispatchRawNotification(const RawNotification& raw) {
  notif_dispatched_.fetch_add(1);
  DispatchHeavy(raw.kind, raw.content);
}

void NwcClient::DispatchHeavy(uint32_t kind, const std::string& content) {
  // Pick variant by kind — modern (23197) ⇒ NIP-44 v2, legacy
  // (23196) ⇒ NIP-04. The spec mandates the same encryption for both
  // sides of a conversation so this match is reliable.
  const nostr::EncryptionVariant variant =
      (kind == kKindNotifModern) ? nostr::EncryptionVariant::kNip44V2
                                 : nostr::EncryptionVariant::kNip04;
  if (!LoadKeys()) return;
  decrypt_attempts_.fetch_add(1);
  nostr::Nip4xDecryptResult dec =
      nostr::Decrypt(variant, seckey_, wallet_pub_, content);
  if (!dec.ok) {
    if (variant == nostr::EncryptionVariant::kNip44V2) {
      decrypt_fail_nip44_.fetch_add(1);
    } else {
      decrypt_fail_nip04_.fetch_add(1);
    }
    return;
  }
  decrypt_ok_.fetch_add(1);

  PaymentNotification pn;
  if (DecodePaymentNotification(dec.plaintext, pn) != RpcError::kOk) {
    decode_notif_fail_.fetch_add(1);
    return;
  }
  decode_notif_ok_.fetch_add(1);
  if (pn.direction == PaymentDirection::kIncoming) {
    // Optimistically bump the cached balance — keeps the displayed
    // sats fresh between get_balance polls. Atomic CAS-free update
    // is fine: we're the sole writer for kIncoming/kOutgoing, and a
    // concurrent OnResponse on the WS task simply overwrites with the
    // wallet's authoritative value next poll.
    balance_msat_cache_.fetch_add(pn.amount_msat);
  } else if (pn.direction == PaymentDirection::kOutgoing) {
    const uint64_t total = pn.amount_msat + pn.fees_paid_msat;
    uint64_t cur = balance_msat_cache_.load();
    uint64_t next = (cur > total) ? (cur - total) : 0ULL;
    balance_msat_cache_.store(next);
  }
  last_pay_direction_.store(static_cast<uint8_t>(pn.direction));
  last_pay_amount_sats_.store(static_cast<int64_t>(pn.amount_msat / 1000ULL));
  last_pay_received_ms_.store(now_ ? now_() * 1000 : 0);
  if (on_payment_) {
    cb_on_payment_dispatched_.fetch_add(1);
    on_payment_(pn);
  }
}

DebugSnapshot NwcClient::GetDebugSnapshot() const {
  DebugSnapshot s;
  s.state = state_;
  s.encryption = encryption_;
  s.balance_msat_cache = balance_msat_cache_.load();
  s.sub_id_info = sub_id_info_;
  s.sub_id_rpc = sub_id_rpc_;
  s.events_total = events_total_.load();
  s.events_info = events_info_.load();
  s.events_response = events_response_.load();
  s.events_notif_modern = events_notif_modern_.load();
  s.events_notif_legacy = events_notif_legacy_.load();
  s.events_other = events_other_.load();
  s.last_kind = last_kind_.load();
  s.last_event_ms = last_event_ms_.load();
  s.last_response_ms = last_response_ms_.load();
  s.decrypt_attempts = decrypt_attempts_.load();
  s.decrypt_ok = decrypt_ok_.load();
  s.decrypt_fail_nip44 = decrypt_fail_nip44_.load();
  s.decrypt_fail_nip04 = decrypt_fail_nip04_.load();
  s.decode_notif_ok = decode_notif_ok_.load();
  s.decode_notif_fail = decode_notif_fail_.load();
  s.decode_resp_ok = decode_resp_ok_.load();
  s.decode_resp_fail = decode_resp_fail_.load();
  s.cb_on_payment_dispatched = cb_on_payment_dispatched_.load();
  s.cb_on_balance_dispatched = cb_on_balance_dispatched_.load();
  s.last_pay_direction = last_pay_direction_.load();
  s.last_pay_amount_sats = last_pay_amount_sats_.load();
  s.last_pay_received_ms = last_pay_received_ms_.load();
  s.notif_enqueued = notif_enqueued_.load();
  s.notif_dropped = notif_dropped_.load();
  s.notif_dispatched = notif_dispatched_.load();
  return s;
}

}  // namespace nwc
}  // namespace btclock
