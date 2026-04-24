#include "nostr/zap_listener.hpp"

#include <utility>

#include "nostr/parser.hpp"

namespace btclock {
namespace nostr {

ZapListener::ZapListener(SubscriptionManager& subs, std::string sub_id,
                         std::string recipient_pubkey_hex)
    : subs_(subs), sub_id_(std::move(sub_id)),
      recipient_(std::move(recipient_pubkey_hex)) {
  // We install a manager-level event callback that dispatches by sub_id
  // here. Callers that need multiple ZapListeners per manager should
  // fan out from one top-level event handler themselves; for now the
  // firmware runs one listener.
  subs_.SetOnEvent([this](const std::string& sid, const Event& ev) {
    if (sid == sub_id_) Handle(sid, ev);
  });
}

ZapListener::~ZapListener() { Stop(); }

bool ZapListener::Start() {
  if (started_) return true;
  Filter f;
  f.kinds.push_back(kKindZapReceipt);
  f.p_tags.push_back(recipient_);
  if (!subs_.Subscribe(sub_id_, f)) return false;
  started_ = true;
  return true;
}

void ZapListener::Stop() {
  if (!started_) return;
  subs_.Unsubscribe(sub_id_);
  started_ = false;
}

void ZapListener::Handle(const std::string& /*sid*/, const Event& ev) {
  if (ev.kind != kKindZapReceipt) return;
  // Drop zero-sat receipts before they reach the callback so the LED
  // flash, screen overlay, and LatestZap snapshot update all stay
  // suppressed. Relays occasionally forward NIP-57 receipts with a
  // malformed or zero `amount` tag; surfacing them briefly flashed
  // the device for a non-event. ShouldSurfaceZap also re-checks kind
  // — harmless given the guard above, meaningful for test ergonomics.
  if (!ShouldSurfaceZap(ev)) return;
  ZapInfo z;
  z.raw = &ev;
  z.content = ev.content;
  ExtractZapAmountMsat(ev, z.amount_msat);
  ExtractZapBolt11(ev, z.bolt11);
  if (on_zap_) on_zap_(z);
}

}  // namespace nostr
}  // namespace btclock
