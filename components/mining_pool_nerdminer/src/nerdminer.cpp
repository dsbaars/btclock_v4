// NerdMiner Pool — solo pool, ckpool fork. The ckpool-family parser
// handles the JSON shape unchanged (same `hashrate1m` SI-suffixed
// string).

#include "mining_pool_nerdminer/nerdminer.hpp"

#include "mining_pool_common/ckpool_family_parser.hpp"
#include "prefs.hpp"

namespace btclock {
namespace mining_pools {

namespace {
constexpr const char* kPrefsNs = "pool";
constexpr const char* kUserKey = "user";
}  // namespace

std::string NerdMinerPoolBase::api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";
  return base_url() + "/users/" + user;
}

bool NerdMinerPoolBase::parse_response(const char* body,
                                       ParsedStats& out) const {
  return ckpool_family::parse(body, out);
}

}  // namespace mining_pools
}  // namespace btclock
