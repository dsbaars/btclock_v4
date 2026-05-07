#include "nostr/zap_listener.hpp"

#include <cstdint>
#include <ctime>
#include <utility>

#include "esp_log.h"
#include "nostr/event_verify.hpp"
#include "nostr/parser.hpp"

namespace btclock {
namespace nostr {
namespace {
constexpr const char* kTag = "nostr-zap";
}  // namespace

ZapListener::ZapListener(SubscriptionManager& subs, std::string sub_id,
                         std::vector<std::string> recipient_pubkeys_hex)
    : subs_(subs),
      sub_id_(std::move(sub_id)),
      recipients_(std::move(recipient_pubkeys_hex)) {
  // We install a manager-level event callback that dispatches by sub_id
  // here. SubscriptionManager holds exactly one callback, so this
  // overwrites any previously installed listener — by design: the
  // firmware has one ZapListener (one zap recipient pubkey per
  // BTClock). Multiple listeners on the same manager would need a
  // caller-side dispatcher.
  subs_.SetOnEvent([this](const std::string& sid, const Event& ev) {
    if (sid == sub_id_) Handle(sid, ev);
  });
}

ZapListener::~ZapListener() {
  Stop();
}

bool ZapListener::Start() {
  if (started_) return true;
  if (recipients_.empty()) return false;
  Filter f;
  f.kinds.push_back(kKindZapReceipt);
  // NIP-01 §filters — `#p` accepts an array of values; the relay
  // returns an event when any one of them matches. NIP-57 zap receipts
  // (kind 9735) tag the recipient with `["p", <pubkey>]`, so a single
  // REQ can cover all configured recipients without extra WSS load.
  f.p_tags = recipients_;
  // `since` bounds stored-event replay on (re)connect to the last
  // kZapMaxAgeSeconds window, and `limit:1` asks for just the newest
  // one. Both are NIP-01 SHOULDs — the arrival-side guard in Handle()
  // backs them up for relays that ignore either. If wall-clock isn't
  // valid yet (pre-SNTP; time() < some threshold), omit `since` so
  // the REQ is still well-formed; the arrival guard will clamp.
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (now > kZapMaxAgeSeconds) {
    f.since = static_cast<uint64_t>(now - kZapMaxAgeSeconds);
  }
  f.limit = 1;
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
  // Schnorr-verify before any side effect (LED, overlay, snapshot).
  // A hostile relay that wants to flash the device with bogus zaps
  // can no longer do so by forging a receipt over the trusted WSS.
  const auto vr = VerifyEvent(ev);
  if (vr != EventVerifyResult::kOk) {
    ESP_LOGW(kTag, "drop unverified zap id=%.16s vr=%u", ev.id.c_str(),
             static_cast<unsigned>(vr));
    return;
  }
  // Drop zero-sat receipts before they reach the callback so the LED
  // flash, screen overlay, and LatestZap snapshot update all stay
  // suppressed. Relays occasionally forward NIP-57 receipts with a
  // malformed or zero `amount` tag; surfacing them briefly flashed
  // the device for a non-event. ShouldSurfaceZap also re-checks kind
  // — harmless given the guard above, meaningful for test ergonomics.
  if (!ShouldSurfaceZap(ev)) return;
  // Age + "only newer than the last one we showed" gate. Backs up the
  // REQ's `since`/`limit:1` — both are NIP-01 SHOULDs, so we cannot
  // trust every relay to honour them. Skip the age check if the local
  // clock hasn't been set yet (pre-SNTP: `now` would be a few seconds
  // past epoch and every real zap would look "decades old").
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (now > kZapMaxAgeSeconds &&
      !ShouldShowZap(now, static_cast<int64_t>(ev.created_at),
                     last_shown_created_at_)) {
    return;
  }
  last_shown_created_at_ = static_cast<int64_t>(ev.created_at);
  ZapInfo z;
  z.raw = &ev;
  z.content = ev.content;
  ExtractZapAmountMsat(ev, z.amount_msat);
  ExtractZapBolt11(ev, z.bolt11);
  if (on_zap_) on_zap_(z);
}

}  // namespace nostr
}  // namespace btclock
