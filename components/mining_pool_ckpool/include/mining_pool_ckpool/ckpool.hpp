// CKPool HTTPS poller (solo.ckpool.org and eusolo.ckpool.org).
//
// Covers both ckpool.org endpoints — the old firmware had a stray
// eu_ckpool.hpp variant that only differed in the base URL; we fold
// that into a single class driven by a base_url() virtual so adding
// future regional mirrors is a one-liner.
//
// Uses the shared ckpool-family parser (same `hashrate1m` string
// shape as Noderunners / Satoshi Radio).
//
// Despite the superficial similarity, ckpool.org is NOT a WebSocket
// API — the endpoint is a plain HTTPS GET returning JSON. The
// orchestrator note about "wrap the connect phase, not steady-state"
// does not apply here; the TLS gate guards esp_http_client_perform
// like every other pool in this tree.

#pragma once

#include <string>

#include "mining_pool_common/pool_base.hpp"

namespace btclock {
namespace mining_pools {

class CKPoolBase : public PoolDataSource {
 public:
  CKPoolBase() = default;

 protected:
  std::string api_url() const override;
  bool parse_response(const char* body, ParsedStats& out) const override;

  // Regional mirror base URL — e.g. "https://solo.ckpool.org" vs
  // "https://eusolo.ckpool.org". No trailing slash.
  virtual std::string base_url() const = 0;
};

class CKPool : public CKPoolBase {
 public:
  CKPool() = default;
  const char* name() const override { return "pool.ckpool"; }

 protected:
  std::string base_url() const override { return "https://solo.ckpool.org"; }
  const char* pool_name() const override { return "ckpool"; }
};

class EUCKPool : public CKPoolBase {
 public:
  EUCKPool() = default;
  const char* name() const override { return "pool.ckpool.eu"; }

 protected:
  std::string base_url() const override {
    return "https://eusolo.ckpool.org";
  }
  // Old firmware used a distinct pool identity so per-pool caches
  // (downloaded logo, settings) don't collide between mirrors. Keep
  // that separation here even though the UI shows the same label.
  const char* pool_name() const override { return "eu_ckpool"; }
};

}  // namespace mining_pools
}  // namespace btclock
