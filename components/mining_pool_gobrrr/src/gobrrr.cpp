// Registered by mining_pool_stats_handler once pool-selection NVS pref
// lands (follow-up).

#include "mining_pool_gobrrr/gobrrr.hpp"

#include "prefs.hpp"

namespace btclock {
namespace mining_pools {

namespace {
constexpr const char* kPrefsNs = "pool";
constexpr const char* kUserKey = "user";
}  // namespace

std::string GoBrrrPool::api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";
  return "https://pool.gobrrr.me/api/client/" + user;
}

}  // namespace mining_pools
}  // namespace btclock
