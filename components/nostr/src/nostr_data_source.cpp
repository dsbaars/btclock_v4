#include "nostr/nostr_data_source.hpp"

#include <utility>

#include "data_core/hub.hpp"
#include "data_core/snapshot.hpp"
#include "esp_log.h"
#include "nostr/event_verify.hpp"
#include "nostr/parser.hpp"
#include "nostr/relay_client.hpp"
#include "nostr/subscription_manager.hpp"

namespace btclock {
namespace nostr {
namespace {
constexpr const char* kTag = "nostr-src";
}  // namespace

NostrDataSource::NostrDataSource(Config cfg) : cfg_(std::move(cfg)) {}

NostrDataSource::~NostrDataSource() {
  Stop();
}

bool NostrDataSource::relay_connected() const {
  return relay_ && relay_->connected();
}

esp_err_t NostrDataSource::Start(DataHub& hub) {
  hub_ = &hub;

  relay_ = std::make_unique<RelayClient>(cfg_.relay_url);
  subs_ = std::make_unique<SubscriptionManager>(*relay_);

  subs_->SetOnEvent(
      [this](const std::string& sid, const Event& ev) { OnEvent(sid, ev); });

  const esp_err_t rc = relay_->Start();
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "relay start failed: %d", rc);
    return rc;
  }

  Filter f;
  f.kinds.push_back(kKindAppData);
  if (!cfg_.author_pubkey_hex.empty()) {
    f.authors.push_back(cfg_.author_pubkey_hex);
  }
  for (const auto& d : cfg_.d_tags) f.d_tags.push_back(d);
  subs_->Subscribe(cfg_.sub_id, f);

  ESP_LOGI(kTag, "nostr data source started on %s (author=%s)",
           cfg_.relay_url.c_str(), cfg_.author_pubkey_hex.c_str());
  return ESP_OK;
}

esp_err_t NostrDataSource::Stop() {
  // Order matters: SubscriptionManager's constructor installs an
  // on_frame_ lambda on the RelayClient that captures `this`. If we
  // destroy subs_ first, the WS task (still running until
  // relay_->Stop() joins it) can dispatch one more frame into the
  // dangling capture and InstructionFetchError-panic on the freed
  // vtable. Send the NIP-01 CLOSE while the socket is still alive,
  // then JOIN the WS task via relay_->Stop() (which calls
  // esp_websocket_client_stop+destroy and waits for the task to
  // terminate), and only THEN drop subs_. Observed Rev B 4.0.0-beta.11
  // crash signature: InstructionFetchError at PC inside .flash.rodata,
  // backtrace pointing at SubscriptionManager's frame-handler lambda
  // dispatched from esp_websocket_client_dispatch_event.
  if (subs_) subs_->Unsubscribe(cfg_.sub_id);
  if (relay_) relay_->Stop();
  subs_.reset();
  relay_.reset();
  hub_ = nullptr;
  return ESP_OK;
}

void NostrDataSource::OnEvent(const std::string& /*sid*/, const Event& ev) {
  // Reject anything that doesn't carry a valid BIP-340 schnorr signature
  // over the canonical id. WSS-to-relay TLS only proves we're talking
  // to the configured relay, not that the publisher actually signed
  // the event we just received.
  const auto vr = VerifyEvent(ev);
  if (vr != EventVerifyResult::kOk) {
    ESP_LOGW(kTag, "drop unverified event id=%.16s vr=%u", ev.id.c_str(),
             static_cast<unsigned>(vr));
    return;
  }

  // We subscribe with kinds=[30078] so the relay should not send other
  // kinds, but the filter is the relay's contract, not ours — guard.
  if (ev.kind != kKindAppData) {
    ESP_LOGD(kTag, "ignoring kind=%u (not app-data)",
             static_cast<unsigned>(ev.kind));
    return;
  }

  const std::string& d = ev.TagValue("d");
  if (d.empty()) {
    ESP_LOGW(kTag, "kind-30078 event without d tag; dropping");
    return;
  }

  // Staleness gate. NIP-78 is replaceable-by-newest on the relay side,
  // but a relay can redeliver older cached events on reconnect before
  // the newest arrives. We keep the newest timestamp we've consumed per
  // `d` slot and drop anything with a lower `created_at`. Ties are
  // accepted: NIP-01 says clients "SHOULD" take the event with the
  // greatest id in that case, but for our slot semantics re-applying
  // the same content is harmless and Merge() suppresses the update.
  const auto it = last_seen_created_at_.find(d);
  if (it != last_seen_created_at_.end() && ev.created_at < it->second) {
    ESP_LOGD(kTag, "stale event d=%s ts=%llu < seen=%llu", d.c_str(),
             static_cast<unsigned long long>(ev.created_at),
             static_cast<unsigned long long>(it->second));
    return;
  }

  DataSnapshot partial;
  if (!ParseNip78Content(d, ev.content, partial)) {
    ESP_LOGW(kTag, "unparseable d=%s content=%.*s", d.c_str(),
             static_cast<int>(ev.content.size()), ev.content.c_str());
    return;
  }

  last_seen_created_at_[d] = ev.created_at;

  if (hub_ != nullptr) hub_->Report(partial);
  ESP_LOGD(kTag, "report d=%s content=%.*s", d.c_str(),
           static_cast<int>(ev.content.size()), ev.content.c_str());
}

}  // namespace nostr
}  // namespace btclock
