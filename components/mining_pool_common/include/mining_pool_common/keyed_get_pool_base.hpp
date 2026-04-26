// Mix-in base for "single API-key in a header" pool sources.
//
// Several pools (ViaBTC, Foundry USA) share an identical auth model:
//   - Plain GET against an HTTPS endpoint.
//   - One header carrying a per-account API key (default "X-API-KEY").
//   - No signing, no timestamp, no nonce.
//   - JSON response.
//
// PoolDataSource already does the GET + PSRAM-buffered body + TLS-gate
// dance. The only thing that varies between Braiins (Pool-Auth-Token)
// and ViaBTC/Foundry (X-API-KEY) is the header name, which the base
// already exposes via auth_header_name(). This file adds a thin
// subclass that:
//   - Forwards api_key() into auth_token() so subclasses can pick a
//     name that matches the upstream API docs (ViaBTC's docs literally
//     call it "API-KEY").
//   - Defaults auth_header_name() to "X-API-KEY". Subclasses can
//     override if a future keyed pool ships a different header name —
//     the only point of having a virtual rather than a constant.
//
// No .cpp file: every override is one line and the helper has no state
// of its own, so a header-only mix-in keeps the link order simple.

#pragma once

#include <string>

#include "mining_pool_common/pool_base.hpp"

namespace btclock {
namespace mining_pools {

class KeyedGetPoolBase : public PoolDataSource {
 public:
  KeyedGetPoolBase() = default;

 protected:
  // The API key value. Subclasses read it from NVS (typically via
  // btclock::Prefs settings(prefs::kSettingsNs).GetString(...)).
  // Empty string makes the base skip the header — matches the existing
  // auth_token() semantic and lets the operator clear the key without
  // triggering a 401 loop.
  virtual std::string api_key() const = 0;

  std::string auth_token() const final { return api_key(); }

  // Default header for the keyed-get family. Override only if a future
  // pool ships under a different name (e.g. "Authorization: ApiKey ...").
  const char* auth_header_name() const override { return "X-API-KEY"; }
};

}  // namespace mining_pools
}  // namespace btclock
