// Registered by mining_pool_stats_handler once pool-selection NVS pref
// lands (follow-up).

#include "mining_pool_ocean/ocean.hpp"

#include "mining_pool_ocean/ocean_parser.hpp"
#include "prefs.hpp"

namespace btclock {
namespace mining_pools {

namespace {
constexpr const char* kPrefsNs = "pool";
constexpr const char* kUserKey = "user";
}  // namespace

std::string OceanPool::api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";  // base class skips the poll
  return "https://api.ocean.xyz/v1/statsnap/" + user;
}

bool OceanPool::parse_response(const char* body, ParsedStats& out) const {
  return ocean::parse(body, out);
}

}  // namespace mining_pools
}  // namespace btclock
