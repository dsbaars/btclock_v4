#include "mining_pool_noderunners/noderunners.hpp"

#include "mining_pool_common/ckpool_family_parser.hpp"
#include "prefs.hpp"

namespace btclock {
namespace mining_pools {

namespace {
constexpr const char* kPrefsNs = "pool";
constexpr const char* kUserKey = "user";
constexpr const char* kGlobalKey = "global";  // bool
constexpr const char* kBase = "https://pool.noderunners.network/api/v1";
}  // namespace

std::string NoderunnersPool::api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  if (prefs.GetBool(kGlobalKey, false)) {
    return std::string(kBase) + "/pool";
  }
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";
  return std::string(kBase) + "/users/" + user;
}

bool NoderunnersPool::parse_response(const char* body, ParsedStats& out) const {
  return ckpool_family::parse(body, out);
}

}  // namespace mining_pools
}  // namespace btclock
