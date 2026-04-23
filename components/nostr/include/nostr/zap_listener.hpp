// NIP-57 zap-receipt listener.
//
// Thin layer on top of SubscriptionManager: filters on kind 9735 with a
// `#p` tag matching the recipient pubkey, extracts the amount (msat)
// and bolt11 from each EVENT, and fires a user-supplied callback.
//
// No invoice parsing, no signature verification (see parser.hpp for the
// rationale). The caller decides what to do with the event — typically
// a LedEvent::kZapFlash plus optional on-screen amount, once that
// screen lands.

#pragma once

#include <functional>
#include <string>

#include "nostr/event.hpp"
#include "nostr/subscription_manager.hpp"

namespace btclock {
namespace nostr {

class ZapListener {
 public:
  struct ZapInfo {
    uint64_t amount_msat = 0;
    std::string bolt11;
    std::string content;  // zapper-supplied message (may be empty)
    const Event* raw = nullptr;  // lifetime: valid only inside callback
  };
  using Callback = std::function<void(const ZapInfo& z)>;

  // `recipient_pubkey_hex` is the pubkey whose zaps we want to hear; the
  // listener filters on `#p == [recipient_pubkey_hex]`. `sub_id` is the
  // NIP-01 subscription identifier — make it unique per relay.
  ZapListener(SubscriptionManager& subs, std::string sub_id,
              std::string recipient_pubkey_hex);
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
  std::string recipient_;
  Callback on_zap_;
  bool started_ = false;
};

}  // namespace nostr
}  // namespace btclock
