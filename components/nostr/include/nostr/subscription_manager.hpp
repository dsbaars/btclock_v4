// Nostr subscription manager (NIP-01 REQ / CLOSE bookkeeping).
//
// Owns a set of active subscriptions and routes relay-pushed EVENT
// frames to the correct callback by sub-id. Knows how to format REQ
// filter frames and resend them on reconnect (so a dropped relay
// socket self-heals without the caller doing anything).
//
// This class does not own a RelayClient — it takes one by reference.
// One manager per relay; use multiple instances for a relay pool.

#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "nostr/event.hpp"

namespace btclock {
namespace nostr {

class RelayClient;

// A filter is a flat map of NIP-01 filter fields. We model only the
// subset this firmware uses: kinds, authors, `#d`, `#p`, since, limit.
// Extending is trivial — see `BuildReqJson` in subscription_manager.cpp.
struct Filter {
  std::vector<uint32_t> kinds;
  std::vector<std::string> authors;  // lowercase hex pubkeys
  std::vector<std::string> d_tags;   // `#d` — NIP-33 parameterized slot
  std::vector<std::string> p_tags;   // `#p` — referenced pubkey (zaps)
  uint64_t since = 0;                // unix seconds; 0 => omit field
  int limit = 0;                     // 0 => omit field
};

class SubscriptionManager {
 public:
  using EventCallback =
      std::function<void(const std::string& sub_id, const Event& ev)>;
  using EoseCallback = std::function<void(const std::string& sub_id)>;

  // Attaches to an existing relay. The manager installs its own text
  // callback on the relay — don't share the relay across multiple
  // managers.
  explicit SubscriptionManager(RelayClient& relay);
  ~SubscriptionManager();

  void SetOnEvent(EventCallback cb) { on_event_ = std::move(cb); }
  void SetOnEose(EoseCallback cb) { on_eose_ = std::move(cb); }

  // Open a subscription with the given filter. If the relay is already
  // connected the REQ is sent immediately; otherwise it's queued and
  // fired on the next connect. Returns false iff `sub_id` is already in
  // use.
  bool Subscribe(const std::string& sub_id, const Filter& filter);

  // Close a subscription. Sends CLOSE to the relay if connected.
  void Unsubscribe(const std::string& sub_id);

  // Called by RelayClient's frame callback. Exposed as public so tests
  // can inject synthetic frames; production wiring is automatic.
  void HandleTextFrame(const char* data, size_t len);

 private:
  // Called by RelayClient on connect/disconnect via our installed hook.
  void OnConnect(bool connected);

  // Resend all active REQs (e.g. after reconnect).
  void ReissueAll();

  RelayClient& relay_;
  std::mutex mu_;
  std::map<std::string, Filter> subs_;  // sub_id -> filter
  EventCallback on_event_;
  EoseCallback on_eose_;
};

// Build a NIP-01 REQ JSON string for the given sub_id + filter. Exposed
// so tests can assert on the exact wire format without spinning up a
// relay.
std::string BuildReqJson(const std::string& sub_id, const Filter& f);

// Build a NIP-01 CLOSE JSON string.
std::string BuildCloseJson(const std::string& sub_id);

}  // namespace nostr
}  // namespace btclock
