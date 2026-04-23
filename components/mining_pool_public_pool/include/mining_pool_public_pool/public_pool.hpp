// Public Pool HTTPS poller.
//
// Polls public-pool.io:40557 (note the non-standard port) every
// minute. Also exposes LocalPublicPool — same response shape, URL
// comes from a user-configured NVS key `pool/local_host` so a home
// solo-miner stack can point BTClock at its local endpoint.
//
// Local variant uses plain HTTP (typical deployment is on the LAN),
// so the TLS gate is still taken but is a no-op fast path — the
// esp_http_client short-circuits TLS for http:// URLs.

#pragma once

#include <string>

#include "mining_pool_common/pool_base.hpp"

namespace btclock {
namespace mining_pools {

class PublicPoolBase : public PoolDataSource {
 public:
  PublicPoolBase() = default;

 protected:
  bool parse_response(const char* body, ParsedStats& out) const override;
  // Public-pool responses list every worker for the account; a user
  // with many bitaxe units can produce a ~20 KB body. Bump cap to
  // 64 KB (still tight enough that a runaway server won't eat heap).
  size_t max_response_bytes() const override { return 64 * 1024; }
};

class PublicPool : public PublicPoolBase {
 public:
  PublicPool() = default;
  const char* name() const override { return "pool.public"; }

 protected:
  std::string api_url() const override;
  const char* pool_name() const override { return "public_pool"; }
};

class LocalPublicPool : public PublicPoolBase {
 public:
  LocalPublicPool() = default;
  const char* name() const override { return "pool.public.local"; }

 protected:
  std::string api_url() const override;
  const char* pool_name() const override { return "local_pool"; }
};

}  // namespace mining_pools
}  // namespace btclock
