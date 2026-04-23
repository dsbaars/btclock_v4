#include "nostr/subscription_manager.hpp"

#include <string>

#include "esp_log.h"
#include "nostr/parser.hpp"
#include "nostr/relay_client.hpp"

namespace btclock {
namespace nostr {
namespace {
constexpr const char* kTag = "nostr-sub";
}  // namespace

SubscriptionManager::SubscriptionManager(RelayClient& relay) : relay_(relay) {
  relay_.SetOnFrame([this](const char* data, size_t len) {
    HandleTextFrame(data, len);
  });
  relay_.SetOnConnect([this](bool connected) { OnConnect(connected); });
}

SubscriptionManager::~SubscriptionManager() = default;

bool SubscriptionManager::Subscribe(const std::string& sub_id,
                                     const Filter& filter) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (subs_.count(sub_id) != 0) return false;
    subs_[sub_id] = filter;
  }
  if (relay_.connected()) {
    const std::string req = BuildReqJson(sub_id, filter);
    relay_.SendText(req);
  }
  return true;
}

void SubscriptionManager::Unsubscribe(const std::string& sub_id) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (subs_.erase(sub_id) == 0) return;
  }
  if (relay_.connected()) {
    const std::string close = BuildCloseJson(sub_id);
    relay_.SendText(close);
  }
}

void SubscriptionManager::OnConnect(bool connected) {
  if (connected) ReissueAll();
}

void SubscriptionManager::ReissueAll() {
  std::map<std::string, Filter> copy;
  {
    std::lock_guard<std::mutex> lk(mu_);
    copy = subs_;
  }
  for (const auto& [sub_id, f] : copy) {
    relay_.SendText(BuildReqJson(sub_id, f));
  }
}

void SubscriptionManager::HandleTextFrame(const char* data, size_t len) {
  Envelope env;
  // Parser takes a std::string; copy once. Relay frames are tiny.
  if (!ParseEnvelope(std::string(data, len), env)) {
    ESP_LOGW(kTag, "dropped unparseable frame (%u bytes)",
             static_cast<unsigned>(len));
    return;
  }
  switch (env.type) {
    case EnvelopeType::kEvent:
      if (on_event_) on_event_(env.sub_id, env.event);
      break;
    case EnvelopeType::kEose:
      if (on_eose_) on_eose_(env.sub_id);
      break;
    case EnvelopeType::kNotice:
      ESP_LOGI(kTag, "NOTICE: %s", env.message.c_str());
      break;
    case EnvelopeType::kClosed:
      ESP_LOGI(kTag, "CLOSED %s: %s", env.sub_id.c_str(),
               env.message.c_str());
      break;
    case EnvelopeType::kOk:
    case EnvelopeType::kUnknown:
    default:
      break;
  }
}

}  // namespace nostr
}  // namespace btclock
