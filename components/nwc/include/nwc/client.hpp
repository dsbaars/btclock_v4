// Nostr Wallet Connect (NIP-47) client state machine.
//
// Composes:
//   * a `RelayClient` (provided — one socket per NwcClient instance)
//   * a `SubscriptionManager` (provided, riding the same RelayClient)
//   * the encryption dispatcher from `nostr/nip4x.hpp`
//   * the schnorr sign helper from `nostr/event_sign.hpp`
//   * the JSON-RPC encode/decode helpers from `nwc/jsonrpc.hpp`
//
// State machine (simplified):
//
//   * kIdle          → caller hasn't called Start() yet, or Stop() was
//                       called.
//   * kBootstrapping → subscription open, INFO event (kind 13194)
//                       awaited.
//   * kReady         → INFO seen, encryption variant locked. Ready to
//                       issue get_balance.
//   * kFatal         → unrecoverable: e.g. bad URI, signer rejects
//                       key. The instance stays useless until
//                       Stop()+reconstruct.
//
// Threading: this class is single-threaded by construction. All
// public methods MUST be called from the relay-client task callback
// (`SubscriptionManager::HandleTextFrame` is already on that task);
// the boot-time setup runs once before Start(). No mutex internally.
//
// Memory: conversation keys (for NIP-44 v2) are cached per instance;
// re-derivation per request would dominate CPU otherwise.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "nostr/event.hpp"
#include "nostr/nip4x.hpp"
#include "nostr/subscription_manager.hpp"
#include "nwc/jsonrpc.hpp"
#include "nwc/queue.hpp"
#include "nwc/uri.hpp"

namespace btclock {

namespace nwc {

enum class State : uint8_t {
  kIdle = 0,
  kBootstrapping,
  kReady,
  kFatal,
};

// Caller-supplied randomness source. On target this is wired to
// `esp_fill_random`; in host tests we plug a counter / fixed buffer
// for determinism.
using RandomBytesFn = std::function<void(uint8_t* out, size_t n)>;

// Caller-supplied wall-clock source (unix seconds). Same trick as
// RandomBytesFn for testability.
using NowSecsFn = std::function<int64_t()>;

// User-facing callbacks. All fire on the relay-client task.
using BalanceCallback = std::function<void(uint64_t balance_msat)>;
using PaymentCallback = std::function<void(const PaymentNotification&)>;
using ReadyCallback = std::function<void(const InfoEvent& info)>;

// Frame-publish hook so the state machine can be tested without an
// actual RelayClient. The default implementation sends through the
// owned RelayClient; host tests inject a capturing functor.
using PublishFn = std::function<bool(const char* data, size_t len)>;

// Subscribe/Unsubscribe hooks — caller wires these to a
// `nostr::SubscriptionManager` in production, or a no-op functor in
// host tests. Splitting them off the SubscriptionManager type keeps
// NwcClient host-testable (the manager pulls in `RelayClient` which
// depends on ESP-IDF symbols).
using SubscribeFn =
    std::function<void(const std::string& sub_id, const nostr::Filter& filter)>;
using UnsubscribeFn = std::function<void(const std::string& sub_id)>;

// Caller-supplied enqueue hook for kind 23197/23196 notifications.
// Returning true is "accepted", false is "dropped" (queue full or
// shut down). When set, `HandleEvent` for the notification kinds runs
// ONLY the light copy+enqueue path — no decrypt, no decode, no
// callback. The worker side runs the heavy path via
// `DispatchRawNotification`. When unset (host tests, or pre-init),
// HandleEvent falls back to synchronous in-task dispatch — preserves
// the legacy contract.
using NotifEnqueueFn = std::function<bool(RawNotification&&)>;

class NwcClient {
 public:
  // `pairing` is the parsed NWC URI. `publish` is the function used
  // to ship the signed event frame to the relay — on target the
  // caller wires it to `RelayClient::SendText`. `subscribe` /
  // `unsubscribe` route through whichever subscription dispatcher
  // owns the relay socket; the caller is responsible for routing
  // inbound events back through `HandleEvent`.
  NwcClient(PairingUri pairing, PublishFn publish, SubscribeFn subscribe,
            UnsubscribeFn unsubscribe);
  ~NwcClient();

  NwcClient(const NwcClient&) = delete;
  NwcClient& operator=(const NwcClient&) = delete;

  void SetRandomFn(RandomBytesFn fn) { random_ = std::move(fn); }
  void SetNowFn(NowSecsFn fn) { now_ = std::move(fn); }
  void SetOnBalance(BalanceCallback fn) { on_balance_ = std::move(fn); }
  void SetOnPayment(PaymentCallback fn) { on_payment_ = std::move(fn); }
  void SetOnReady(ReadyCallback fn) { on_ready_ = std::move(fn); }
  // Wire the notification handoff. With this set, `HandleEvent` for
  // kind 23197/23196 only enqueues — the heavy decrypt/decode/dispatch
  // runs on whoever calls `DispatchRawNotification` (typically a
  // dedicated worker task). Must be called before `Start()`.
  void SetNotifEnqueueFn(NotifEnqueueFn fn) { notif_enqueue_ = std::move(fn); }

  // Default encryption to use when the wallet hasn't yet sent its
  // INFO event. NIP-47 says "absence of encryption tag → assume
  // nip04" — so the safe default is nip04, but every modern wallet
  // ships nip44_v2 within seconds of subscribe. We start in nip44_v2
  // and downgrade to nip04 if INFO advertises only that.
  void SetInitialEncryption(nostr::EncryptionVariant v) { encryption_ = v; }

  // Open the subscription. Picks a sub-id derived from the wallet
  // pubkey so multi-NwcClient hosts (unlikely on this device, but
  // possible) don't collide.
  void Start();
  void Stop();

  // Build, sign, encrypt and publish a `get_balance` request. The
  // response will arrive as a kind 23195 event; matching is by the
  // signed event id (kept in `inflight_request_id_`). Returns true
  // if the publish hook accepted the frame.
  bool RequestGetBalance();

  // Build, sign, encrypt and publish a `list_transactions` request
  // bounded to [from_secs, now]. Limit caps the response size — keep
  // it small (~20) since the response array translates directly into
  // synthetic on_payment_ callbacks and screen overlays. Used by the
  // boot poll path to surface payments that landed while the device
  // was offline (NIP-47's push-notification stream only covers events
  // received since subscription open). One-shot — caller decides when
  // to fire; the response routes through OnResponse → DispatchHeavy
  // per-tx so existing balance/snapshot/screen plumbing reuses.
  bool RequestListTransactions(int64_t from_secs, uint32_t limit = 20);

  State state() const { return state_; }
  nostr::EncryptionVariant encryption() const { return encryption_; }
  uint64_t balance_msat_cache() const { return balance_msat_cache_.load(); }
  const std::string& wallet_pubkey() const {
    return pairing_.wallet_pubkey_hex;
  }
  const std::string& last_request_id() const { return inflight_request_id_; }
  const std::string& sub_id_info() const { return sub_id_info_; }
  const std::string& sub_id_rpc() const { return sub_id_rpc_; }

  // Test hook — feed a kind-23195 response / kind-23197 (or 23196
  // legacy) notification / kind-13194 INFO event without going
  // through the relay. The caller must have populated the Event's
  // id/pubkey/kind/tags/content fields per NIP-01. Signature is NOT
  // re-verified here — we trust the caller (production wires this
  // behind SubscriptionManager which re-verifies via
  // VerifyEvent at the data-source seam).
  void HandleEvent(const nostr::Event& ev);

  // Worker-side entry. Runs the heavy decrypt + decode + callback for
  // a previously-enqueued notification. MUST NOT be called from the
  // WS RX task (the whole point of the queue is to keep that task's
  // stack clean). Safe to call from any other task; internally
  // touches only atomic counters and the (now atomic) balance cache.
  void DispatchRawNotification(const RawNotification& raw);

 private:
  bool PublishSignedRequest(const std::string& plaintext_payload);
  void OnInfoEvent(const nostr::Event& ev);
  void OnResponse(const nostr::Event& ev);
  // Light path on the WS task: copy → enqueue. When notif_enqueue_
  // is unset, falls through to the legacy synchronous dispatch.
  void OnNotification(const nostr::Event& ev);
  // The heavy path, shared between the worker-driven flow and the
  // legacy synchronous path. Takes the already-decoupled kind+
  // ciphertext rather than an Event reference so the worker side can
  // own the raw envelope without re-parsing.
  void DispatchHeavy(uint32_t kind, const std::string& content);

  // Apply a settled payment to the local balance cache, then fan it
  // out through on_payment_. Same call shape regardless of whether
  // the payment came from a live kind-23197/23196 notification or a
  // synthetic boot-poll list_transactions entry.
  void ApplyPaymentToBalance(const PaymentNotification& pn);

  // Dedup guard for ApplyPaymentToBalance. Alby Hub emits BOTH a legacy
  // (23196 / NIP-04) AND a modern (23197 / NIP-44) notification for the
  // same payment, and the boot-poll list_transactions replay can
  // re-surface a just-notified one — all carrying the same
  // payment_hash. Returns true (and records the hash) if this hash was
  // already seen within the dedup window, so the caller drops the
  // duplicate instead of double-firing on_payment_ (double frontlight
  // flash / overlay) and double-counting the optimistic balance add.
  bool IsDuplicatePayment(const std::string& payment_hash);

  // Derive seckey32 / pubkey32 from the URI. Cached on the instance
  // since they're needed on every send.
  bool LoadKeys();

  // Compute the NIP-44 v2 conversation key once and cache it.
  bool EnsureConversationKey();

  PairingUri pairing_;
  PublishFn publish_;
  SubscribeFn subscribe_;
  UnsubscribeFn unsubscribe_;
  NotifEnqueueFn notif_enqueue_;

  // Raw 32-byte keys decoded from the URI. Valid iff `keys_loaded_`.
  uint8_t seckey_[32]{};
  uint8_t wallet_pub_[32]{};
  uint8_t conversation_key_[32]{};  // NIP-44 v2 cached key
  bool keys_loaded_ = false;
  bool conv_key_loaded_ = false;

  nostr::EncryptionVariant encryption_ = nostr::EncryptionVariant::kNip44V2;

  // Two stable sub ids on the same relay socket: one fetches the
  // wallet's INFO (kind 13194 by `authors=[wallet_pubkey]` — INFO has
  // no `p` tag so a `#p` filter would never match it), the other
  // carries responses + payment notifications (kinds 23195/23197/
  // 23196 by `#p=[us]`). Folding both into a single Filter dropped
  // INFO on every relay and stranded the client in kBootstrapping —
  // RequestGetBalance is gated on kReady so the poll tick never
  // fired. bd btclock_v4-lwf.6.
  std::string sub_id_info_;
  std::string sub_id_rpc_;
  // Tracks the event id of the most recent request we sent — every
  // kind 23195 response carries an `e` tag pointing at this. Empty
  // means no request in flight.
  std::string inflight_request_id_;
  std::string inflight_method_;  // "get_balance" / "get_info" etc.

  // Updated by OnResponse (WS task) and DispatchHeavy (worker task)
  // post-queue. Atomic so the get-balance snapshot path is race-free.
  std::atomic<uint64_t> balance_msat_cache_{0};
  State state_ = State::kIdle;

  // Recent-payment_hash ring for IsDuplicatePayment. Touched from both
  // the worker task (live notifications) and the WS task (boot-poll
  // list_transactions response), so it carries its own small mutex —
  // the class is otherwise lock-free, but these two paths genuinely
  // race for the duplicate-payment case.
  struct SeenPayment {
    std::string hash;
    uint64_t ts_secs = 0;
  };
  static constexpr size_t kDedupRing = 8;
  static constexpr uint64_t kDedupWindowSecs = 120;
  std::array<SeenPayment, kDedupRing> dedup_ring_{};
  size_t dedup_next_ = 0;
  std::mutex dedup_mu_;

  RandomBytesFn random_;
  NowSecsFn now_;
  BalanceCallback on_balance_;
  PaymentCallback on_payment_;
  ReadyCallback on_ready_;
};

// Convert the URI's hex pubkey/secret to 32-byte arrays. Returns
// false if hex isn't well-formed (shouldn't happen — ParsePairingUri
// already validated — but defensive).
bool DecodeHex32(const std::string& hex, uint8_t out[32]);

}  // namespace nwc
}  // namespace btclock
