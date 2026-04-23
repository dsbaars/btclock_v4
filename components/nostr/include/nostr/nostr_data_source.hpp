// NostrDataSource — DataHub source fed by a Nostr relay.
//
// Subscribes to parameterized-replaceable kind-30078 events (NIP-78)
// from a given publisher pubkey, optionally narrowed to a set of `d`
// tag slots. On each EVENT the source parses the tag/content pair per
// ws-nostr-publish/docs/NOSTR.md and reports a partial DataSnapshot:
//
//   d=blockheight  → snapshot.block_height = stoi(content)
//   d=medianFee    → snapshot.block_fee    = stoi(content)
//                    snapshot.block_fee_precise = stod(content)
//   d=price:<CCY>  → snapshot.prices[<CCY>] = content  (string)
//
// This file only owns the DataSource plumbing — relay socket, manager,
// and sub-id. The parse logic is intentionally left as a TODO callback
// stub until the full decoder lands. Wiring is correct today; the
// hub just receives empty partials.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "data_core/source.hpp"
#include "esp_err.h"
#include "nostr/event.hpp"

namespace btclock {
namespace nostr {

class RelayClient;
class SubscriptionManager;

class NostrDataSource : public DataSource {
 public:
  struct Config {
    std::string relay_url;
    std::string author_pubkey_hex;     // publisher pubkey
    std::string sub_id = "btclock-v1"; // local NIP-01 sub identifier
    // Optional — narrow to specific `d` slots. Empty => all slots
    // published under the given pubkey (price:*, blockheight, medianFee).
    std::vector<std::string> d_tags;
  };

  explicit NostrDataSource(Config cfg);
  ~NostrDataSource() override;

  const char* name() const override { return "nostr-ws"; }
  esp_err_t Start(DataHub& hub) override;
  esp_err_t Stop() override;

 private:
  // Route a decoded EVENT frame into the DataHub. Filters to kind
  // 30078, drops stale replays by (d_tag, created_at), delegates the
  // actual content → snapshot shape work to ParseNip78Content.
  void OnEvent(const std::string& sub_id, const Event& ev);

  Config cfg_;
  DataHub* hub_ = nullptr;
  std::unique_ptr<RelayClient> relay_;
  std::unique_ptr<SubscriptionManager> subs_;
  // Last consumed event timestamp per `d` tag. NIP-78 is replaceable-
  // by-newest, but nothing stops a relay from redelivering an older
  // event after reconnect (or from misbehaving) — we enforce the
  // invariant on our side. Small map (one entry per slot we see).
  std::map<std::string, uint64_t> last_seen_created_at_;
};

}  // namespace nostr
}  // namespace btclock
