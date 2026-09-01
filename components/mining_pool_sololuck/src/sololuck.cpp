// SoloLuck — 0%-fee true-solo pool on a patched ckpool core. The
// ckpool-family parser handles the JSON shape unchanged (same
// `hashrate1m` SI-suffixed string).

#include "mining_pool_sololuck/sololuck.hpp"

#include "mining_pool_common/ckpool_family_parser.hpp"
#include "prefs.hpp"

namespace btclock {
namespace mining_pools {

namespace {
constexpr const char* kPrefsNs = "pool";
constexpr const char* kUserKey = "user";
}  // namespace

std::string SoloLuckPool::api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";
  return "https://sololuck.io/users/" + user;
}

bool SoloLuckPool::parse_response(const char* body, ParsedStats& out) const {
  return ckpool_family::parse(body, out);
}

}  // namespace mining_pools
}  // namespace btclock
