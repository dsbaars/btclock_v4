#include "nostr/nostr_data_source.hpp"

#include <utility>

#include "esp_log.h"
#include "nostr/relay_client.hpp"
#include "nostr/subscription_manager.hpp"

namespace btclock {
namespace nostr {
namespace {
constexpr const char* kTag = "nostr-src";
}  // namespace

NostrDataSource::NostrDataSource(Config cfg) : cfg_(std::move(cfg)) {}

NostrDataSource::~NostrDataSource() { Stop(); }

esp_err_t NostrDataSource::Start(DataHub& hub) {
  hub_ = &hub;

  relay_ = std::make_unique<RelayClient>(cfg_.relay_url);
  subs_ = std::make_unique<SubscriptionManager>(*relay_);

  subs_->SetOnEvent([this](const std::string& sid, const Event& ev) {
    OnEvent(sid, ev);
  });

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
  if (subs_) subs_->Unsubscribe(cfg_.sub_id);
  subs_.reset();
  if (relay_) relay_->Stop();
  relay_.reset();
  hub_ = nullptr;
  return ESP_OK;
}

void NostrDataSource::OnEvent(const std::string& /*sid*/, const Event& ev) {
  // TODO(btclock_v3_fci-0wm): dispatch by `d` tag into DataSnapshot per
  // NOSTR.md:
  //   d=blockheight  → block_height  (content is decimal integer string)
  //   d=medianFee    → block_fee + block_fee_precise (decimal string)
  //   d=price:<CCY>  → prices["<CCY>"] (decimal string, stored verbatim)
  // Keep this stub wired so the relay + subscription infra is exercised
  // end-to-end; parsing follows once the wire format is confirmed
  // against a live relay.
  ESP_LOGI(kTag, "event kind=%u d=%s content=%.*s",
           static_cast<unsigned>(ev.kind),
           ev.TagValue("d").c_str(),
           static_cast<int>(ev.content.size()), ev.content.c_str());
}

}  // namespace nostr
}  // namespace btclock
