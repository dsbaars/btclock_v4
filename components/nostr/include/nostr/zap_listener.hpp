// NIP-57 zap-receipt listener.
//
// Thin layer on top of SubscriptionManager: filters on kind 9735 with a
// `#p` tag matching the recipient pubkey, extracts the amount (msat)
// and bolt11 from each EVENT, and fires a user-supplied callback.
//
// Schnorr-verifies every kind 9735 event via VerifyEvent before
// firing the callback (relays are not trusted). No invoice parsing —
// the caller decides what to do with the verified event, typically a
// LedEffect::kZapFlash plus optional on-screen amount.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nostr/event.hpp"
#include "nostr/subscription_manager.hpp"

namespace btclock {
namespace nostr {

// Maximum age (in seconds) of a zap receipt we are willing to surface.
// Doubles as the NIP-01 REQ `since` window and as a defensive host-side
// drop gate on arrival: stored-event replays from buggy/permissive
// relays — or mis-timed local clocks — would otherwise flash the device
// through old receipts on every reconnect. 15 minutes matches the
// user-visible "latest zap" semantics.
constexpr int64_t kZapMaxAgeSeconds = 15 * 60;

// Pure-logic decision: given wall-clock `now` (unix seconds), the
// event's `event_created_at`, and the `last_shown_created_at` of the
// most recent zap we already surfaced (0 if none), should this zap be
// rendered?
//
// Rules (each test case in test_nostr_parse.cpp):
//   - Future-dated events (created_at > now, e.g. clock skew) are
//     clamped to "fresh enough" — we don't drop on clock skew.
//   - Events older than `kZapMaxAgeSeconds` are dropped.
//   - Exactly at the cutoff (age == kZapMaxAgeSeconds) is kept
//     (inclusive on the lower bound, so a zap from 15 min 0 s ago
//     still surfaces).
//   - Events with `created_at <= last_shown_created_at` are dropped
//     (only strictly-newer zaps win). `last_shown_created_at == 0`
//     disables the dedupe check.
//
// Defined inline so host tests can pull it in via the header alone,
// without linking the ZapListener translation unit (which references
// SubscriptionManager methods the host tests don't build).
inline bool ShouldShowZap(int64_t now, int64_t event_created_at,
                          int64_t last_shown_created_at) {
  if (last_shown_created_at > 0 && event_created_at <= last_shown_created_at) {
    return false;
  }
  if (event_created_at > now) return true;
  const int64_t age = now - event_created_at;
  return age <= kZapMaxAgeSeconds;
}

class ZapListener {
 public:
  struct ZapInfo {
    uint64_t amount_msat = 0;
    std::string bolt11;
    std::string content;         // zapper-supplied message (may be empty)
    const Event* raw = nullptr;  // lifetime: valid only inside callback
  };
  using Callback = std::function<void(const ZapInfo& z)>;

  // `recipient_pubkeys_hex` is the set of pubkeys whose zaps we want to
  // hear; the listener filters on `#p` containing every entry — NIP-01
  // permits multiple values on a single tag filter, so all recipients
  // share one REQ (no extra WSS, no extra subscription). Empty input
  // is treated as "no recipients" — Start() will refuse. `sub_id` is
  // the NIP-01 subscription identifier — make it unique per relay.
  ZapListener(SubscriptionManager& subs, std::string sub_id,
              std::vector<std::string> recipient_pubkeys_hex);
  ~ZapListener();

  void SetOnZap(Callback cb) { on_zap_ = std::move(cb); }

  // Open the subscription. Idempotent: a second call is a no-op.
  bool Start();

  // Close the subscription.
  void Stop();

 private:
  void Handle(const std::string& sub_id, const Event& ev);

  SubscriptionManager& subs_;
  std::string sub_id_;
  std::vector<std::string> recipients_;
  Callback on_zap_;
  bool started_ = false;
  // Tracks the created_at of the last zap we passed to `on_zap_`.
  // Used to drop duplicate / out-of-order stored-event replays that
  // slip past the relay's `limit:1` (per NIP-01 `limit` is SHOULD,
  // not MUST — a permissive relay can still send several).
  int64_t last_shown_created_at_ = 0;
};

}  // namespace nostr
}  // namespace btclock
