// Registered by mining_pool_stats_handler once pool-selection NVS pref
// lands (follow-up).

#include "mining_pool_ckpool/ckpool.hpp"

#include "mining_pool_common/ckpool_family_parser.hpp"
#include "prefs.hpp"

namespace btclock {
namespace mining_pools {

namespace {
constexpr const char* kPrefsNs = "pool";
constexpr const char* kUserKey = "user";
}  // namespace

std::string CKPoolBase::api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";
  return base_url() + "/users/" + user;
}

bool CKPoolBase::parse_response(const char* body, ParsedStats& out) const {
  return ckpool_family::parse(body, out);
}

}  // namespace mining_pools
}  // namespace btclock
