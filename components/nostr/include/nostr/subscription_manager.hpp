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

#include <atomic>
#include <cstdint>
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

  // Debug counter: number of times ReissueAll() has fired since
  // construction. A non-zero value indicates the relay socket has
  // bounced at least once and the manager re-shipped every active REQ
  // — useful for the /api/nwc/debug snapshot when investigating
  // "events never reach the device" complaints.
  uint32_t reissue_count() const { return reissue_count_.load(); }
  // Number of frames the parser swallowed. parse_fail_ counts
  // frames that failed ParseEnvelope (truncated JSON, malformed
  // shape); event_total_ counts EnvelopeType::kEvent that reached
  // the on_event_ dispatch (or would have, if it were wired).
  uint32_t parse_fail_count() const { return parse_fail_count_.load(); }
  uint32_t event_dispatch_count() const {
    return event_dispatch_count_.load();
  }
  // Sub id of the last EVENT envelope routed to on_event_. Empty
  // before the first event lands. Lets the debug snapshot prove the
  // RPC subscription actually delivered events vs. the response
  // arrived via some other relay-side routing.
  std::string last_event_sub_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_event_sub_id_;
  }
  // First N bytes of the most recent frame ParseEnvelope rejected.
  // Lets the debug snapshot reveal whether the relay is shipping
  // something the parser doesn't recognise (NIP-42 AUTH challenges,
  // NIP-20 OK ack with unexpected shape, …) without a serial console.
  std::string last_parse_fail_head() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_parse_fail_head_;
  }

 private:
  // Called by RelayClient on connect/disconnect via our installed hook.
  void OnConnect(bool connected);

  // Resend all active REQs (e.g. after reconnect).
  void ReissueAll();

  RelayClient& relay_;
  mutable std::mutex mu_;
  std::map<std::string, Filter> subs_;  // sub_id -> filter
  std::string last_event_sub_id_;
  std::string last_parse_fail_head_;
  EventCallback on_event_;
  EoseCallback on_eose_;
  std::atomic<uint32_t> reissue_count_{0};
  std::atomic<uint32_t> parse_fail_count_{0};
  std::atomic<uint32_t> event_dispatch_count_{0};
};

// Build a NIP-01 REQ JSON string for the given sub_id + filter. Exposed
// so tests can assert on the exact wire format without spinning up a
// relay.
std::string BuildReqJson(const std::string& sub_id, const Filter& f);

// Build a NIP-01 CLOSE JSON string.
std::string BuildCloseJson(const std::string& sub_id);

}  // namespace nostr
}  // namespace btclock
