// NerdMiner Pool HTTPS poller (pool.nerdminers.org and pool.nerdminer.io).
//
// NerdMiner Pool is a ckpool fork tuned for ultra-low-difficulty
// devices (NerdMiner / NerdAxe / similar). The HTTP API is
// path-compatible with upstream solo ckpool — `/users/<address>`
// returns JSON with the same `hashrate1m` SI-suffixed string — so the
// shared ckpool-family parser handles it unchanged.
//
// Two cousin pools are supported: `pool.nerdminers.org` and
// `pool.nerdminer.io`. They run the same software and ship the same
// JSON shape; the only difference is the host. Mirrors the ckpool
// CKPool / EUCKPool two-id pattern: a base class plus a `base_url()`
// virtual so adding future regional mirrors is a one-liner.
//
// No upstream logo (verified against
// https://git.btclock.dev/btclock/mining-pool-logos as of 2026-04-26),
// so we leave the pool_base.hpp logo getters at their defaults — the
// renderer paints the text-split fallback the same way it does for
// ckpool and satoshi_radio.

#pragma once

#include <string>

#include "mining_pool_common/pool_base.hpp"

namespace btclock {
namespace mining_pools {

class NerdMinerPoolBase : public PoolDataSource {
 public:
  NerdMinerPoolBase() = default;

 protected:
  std::string api_url() const override;
  bool parse_response(const char* body, ParsedStats& out) const override;

  // Regional mirror base URL — e.g. "https://pool.nerdminers.org" vs
  // "https://pool.nerdminer.io". No trailing slash.
  virtual std::string base_url() const = 0;

  // Solo pool — every NerdMiner mirror only reports hashrate, never a
  // per-user daily payout. Declared once on the base so both .org and
  // .io inherit the same capability without drift.
  bool SupportsDailyEarnings() const override { return false; }
};

class NerdMinersOrgPool : public NerdMinerPoolBase {
 public:
  NerdMinersOrgPool() = default;
  const char* name() const override { return "pool.nerdminers.org"; }

 protected:
  std::string base_url() const override {
    return "https://pool.nerdminers.org";
  }
  const char* pool_name() const override { return "nerdminers_org"; }
};

class NerdMinerIoPool : public NerdMinerPoolBase {
 public:
  NerdMinerIoPool() = default;
  const char* name() const override { return "pool.nerdminer.io"; }

 protected:
  std::string base_url() const override { return "https://pool.nerdminer.io"; }
  // Distinct identity so per-pool caches (settings, downloaded logo
  // when/if upstream gains one) don't collide between mirrors. Mirrors
  // the ckpool / eu_ckpool split.
  const char* pool_name() const override { return "nerdminer_io"; }
};

}  // namespace mining_pools
}  // namespace btclock
