// Gobrrr Pool HTTPS poller.
//
// Same `{"workers": [...]}` response shape as public-pool.io — Gobrrr
// runs the same upstream (public-pool), just under its own domain.
// Reuses the public_pool parser. Different logo / label, handled by
// the pool-selection renderer (follow-up).
//
// Depends on the mining_pool_public_pool component for PublicPoolBase
// (which carries the parser + the 64 KB response cap).

#pragma once

#include <string>

#include "mining_pool_public_pool/public_pool.hpp"

namespace btclock {
namespace mining_pools {

class GoBrrrPool : public PublicPoolBase {
 public:
  GoBrrrPool() = default;
  const char* name() const override { return "pool.gobrrr"; }

 protected:
  std::string api_url() const override;
  const char* pool_name() const override { return "gobrrr_pool"; }
};

}  // namespace mining_pools
}  // namespace btclock
