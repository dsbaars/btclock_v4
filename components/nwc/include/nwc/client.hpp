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

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nostr/event.hpp"
#include "nostr/nip4x.hpp"
#include "nostr/subscription_manager.hpp"
#include "nwc/jsonrpc.hpp"
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
using PublishFn =
    std::function<bool(const char* data, size_t len)>;

// Subscribe/Unsubscribe hooks — caller wires these to a
// `nostr::SubscriptionManager` in production, or a no-op functor in
// host tests. Splitting them off the SubscriptionManager type keeps
// NwcClient host-testable (the manager pulls in `RelayClient` which
// depends on ESP-IDF symbols).
using SubscribeFn = std::function<void(const std::string& sub_id,
                                       const nostr::Filter& filter)>;
using UnsubscribeFn = std::function<void(const std::string& sub_id)>;

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

  // Default encryption to use when the wallet hasn't yet sent its
  // INFO event. NIP-47 says "absence of encryption tag → assume
  // nip04" — so the safe default is nip04, but every modern wallet
  // ships nip44_v2 within seconds of subscribe. We start in nip44_v2
  // and downgrade to nip04 if INFO advertises only that.
  void SetInitialEncryption(nostr::EncryptionVariant v) {
    encryption_ = v;
  }

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

  // Same shape, for the bootstrap `get_info` request. Called once
  // when the kind 13194 INFO event arrives (or, if it doesn't show
  // up after the subscription's stored-event flush, as a fallback).
  bool RequestGetInfo();

  State state() const { return state_; }
  nostr::EncryptionVariant encryption() const { return encryption_; }
  uint64_t balance_msat_cache() const { return balance_msat_cache_; }
  const std::string& wallet_pubkey() const {
    return pairing_.wallet_pubkey_hex;
  }
  const std::string& last_request_id() const { return inflight_request_id_; }

  // Test hook — feed a kind-23195 response / kind-23197 (or 23196
  // legacy) notification / kind-13194 INFO event without going
  // through the relay. The caller must have populated the Event's
  // id/pubkey/kind/tags/content fields per NIP-01. Signature is NOT
  // re-verified here — we trust the caller (production wires this
  // behind SubscriptionManager which re-verifies via
  // VerifyEvent at the data-source seam).
  void HandleEvent(const nostr::Event& ev);

 private:
  bool PublishSignedRequest(const std::string& plaintext_payload);
  void OnInfoEvent(const nostr::Event& ev);
  void OnResponse(const nostr::Event& ev);
  void OnNotification(const nostr::Event& ev);

  // Derive seckey32 / pubkey32 from the URI. Cached on the instance
  // since they're needed on every send.
  bool LoadKeys();

  // Compute the NIP-44 v2 conversation key once and cache it.
  bool EnsureConversationKey();

  PairingUri pairing_;
  PublishFn publish_;
  SubscribeFn subscribe_;
  UnsubscribeFn unsubscribe_;

  // Raw 32-byte keys decoded from the URI. Valid iff `keys_loaded_`.
  uint8_t seckey_[32]{};
  uint8_t wallet_pub_[32]{};
  uint8_t conversation_key_[32]{};  // NIP-44 v2 cached key
  bool keys_loaded_ = false;
  bool conv_key_loaded_ = false;

  nostr::EncryptionVariant encryption_ = nostr::EncryptionVariant::kNip44V2;

  // SubscriptionManager sub id we registered under. Stable so we can
  // CLOSE on Stop().
  std::string sub_id_;
  // Tracks the event id of the most recent request we sent — every
  // kind 23195 response carries an `e` tag pointing at this. Empty
  // means no request in flight.
  std::string inflight_request_id_;
  std::string inflight_method_;  // "get_balance" / "get_info" etc.

  uint64_t balance_msat_cache_ = 0;
  State state_ = State::kIdle;

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
