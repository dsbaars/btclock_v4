#include "io/mining_pool_selector.hpp"

#include <string>

#include "esp_log.h"
#include "mining_pool_braiins/braiins.hpp"
#include "mining_pool_ckpool/ckpool.hpp"
#include "mining_pool_common/pool_base.hpp"
#include "mining_pool_foundry/foundry.hpp"
#include "mining_pool_gobrrr/gobrrr.hpp"
#include "mining_pool_nerdminer/nerdminer.hpp"
#include "mining_pool_noderunners/noderunners.hpp"
#include "mining_pool_ocean/ocean.hpp"
#include "mining_pool_public_pool/public_pool.hpp"
#include "mining_pool_satoshi_radio/satoshi_radio.hpp"
#include "mining_pool_viabtc/viabtc.hpp"
#include "pool_logo_fetcher/pool_logo_fetcher.hpp"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace mining_pools {

namespace {

constexpr const char* kTag = "pool.selector";

// NVS namespace and short keys read by the PoolDataSource subclasses.
constexpr const char* kPoolNs = "pool";
constexpr const char* kPoolUserKey = "user";
constexpr const char* kPoolGlobalKey = "global";
constexpr const char* kPoolLocalHostKey = "local_host";

// Default pool name matches the old firmware's
// DEFAULT_MINING_POOL_NAME ("ocean") so an in-place upgrade picks the
// same pool without the user touching settings.
constexpr const char* kDefaultPoolName = "ocean";

// Sync the /api/settings-owned pool user + global-stats + local-host
// values into the short-keyed `pool` namespace the source classes
// consult each poll. Called once at boot — live PATCHes take effect
// after reboot, matching the old firmware's setupDataSource() timing.
void MirrorSettingsIntoPoolNs() {
  btclock::Prefs settings(btclock::prefs::kSettingsNs);
  btclock::Prefs pool(kPoolNs);

  const std::string user =
      settings.GetString(btclock::prefs::kMiningPoolUser, "");
  pool.SetString(kPoolUserKey, user.c_str());

  const bool global_stats = settings.GetBool(btclock::prefs::kPoolGlobalStats,
                                              false);
  pool.SetBool(kPoolGlobalKey, global_stats);

  const std::string local_host =
      settings.GetString(btclock::prefs::kLocalPoolHost, "");
  if (!local_host.empty()) {
    pool.SetString(kPoolLocalHostKey, local_host.c_str());
  }

  pool.Commit();
}

std::unique_ptr<DataSource> BuildByName(const std::string& name) {
  if (name == "ocean") return std::make_unique<OceanPool>();
  if (name == "noderunners") return std::make_unique<NoderunnersPool>();
  if (name == "satoshi_radio") return std::make_unique<SatoshiRadioPool>();
  if (name == "braiins") return std::make_unique<BraiinsPool>();
  if (name == "public_pool") return std::make_unique<PublicPool>();
  if (name == "local_public_pool") return std::make_unique<LocalPublicPool>();
  if (name == "gobrrr_pool") return std::make_unique<GoBrrrPool>();
  if (name == "ckpool") return std::make_unique<CKPool>();
  if (name == "eu_ckpool") return std::make_unique<EUCKPool>();
  if (name == "nerdminers_org") return std::make_unique<NerdMinersOrgPool>();
  if (name == "nerdminer_io") return std::make_unique<NerdMinerIoPool>();
  if (name == "foundry_usa") return std::make_unique<FoundryPool>();
  if (name == "viabtc") return std::make_unique<ViaBtcPool>();
  return nullptr;
}

// Construct a throwaway PoolDataSource to query its static capability.
// Construction is cheap (no NVS I/O until Start() is called) and we let
// the unique_ptr drop it before returning — the factory dispatch already
// covers every concrete pool, so the capability reflects whatever the
// plugin itself declares rather than a side table that could rot.
std::unique_ptr<PoolDataSource> BuildPoolByName(const std::string& name) {
  if (name == "ocean") return std::make_unique<OceanPool>();
  if (name == "noderunners") return std::make_unique<NoderunnersPool>();
  if (name == "satoshi_radio") return std::make_unique<SatoshiRadioPool>();
  if (name == "braiins") return std::make_unique<BraiinsPool>();
  if (name == "public_pool") return std::make_unique<PublicPool>();
  if (name == "local_public_pool") return std::make_unique<LocalPublicPool>();
  if (name == "gobrrr_pool") return std::make_unique<GoBrrrPool>();
  if (name == "ckpool") return std::make_unique<CKPool>();
  if (name == "eu_ckpool") return std::make_unique<EUCKPool>();
  if (name == "nerdminers_org") return std::make_unique<NerdMinersOrgPool>();
  if (name == "nerdminer_io") return std::make_unique<NerdMinerIoPool>();
  if (name == "foundry_usa") return std::make_unique<FoundryPool>();
  if (name == "viabtc") return std::make_unique<ViaBtcPool>();
  return nullptr;
}

}  // namespace

std::vector<std::string> AvailablePoolNames() {
  // Order mirrors PoolFactory::getAvailablePools() in the old firmware
  // so the WebUI dropdown renders identically on upgrade. New ckpool-
  // family forks (NerdMiner) are appended after the original ckpool
  // entries to keep the existing pool order stable.
  return {"ocean",         "noderunners",     "satoshi_radio", "braiins",
          "public_pool",   "local_public_pool", "gobrrr_pool",
          "ckpool",        "eu_ckpool",       "nerdminers_org",
          "nerdminer_io",  "foundry_usa",     "viabtc"};
}

std::unique_ptr<DataSource> MakeActivePoolSource() {
  btclock::Prefs settings(btclock::prefs::kSettingsNs);
  const bool enabled =
      settings.GetBool(btclock::prefs::kMiningPoolStats, false);
  if (!enabled) {
    ESP_LOGI(kTag, "mining-pool stats disabled (settings/%s=false)",
             btclock::prefs::kMiningPoolStats);
    return nullptr;
  }

  const std::string pool_name =
      settings.GetString(btclock::prefs::kMiningPoolName, kDefaultPoolName);
  if (pool_name.empty()) {
    ESP_LOGW(kTag, "mining-pool stats enabled but %s is empty; skipping",
             btclock::prefs::kMiningPoolName);
    return nullptr;
  }

  MirrorSettingsIntoPoolNs();

  auto src = BuildByName(pool_name);
  if (!src) {
    ESP_LOGW(kTag, "unknown pool '%s' — not in AvailablePoolNames()",
             pool_name.c_str());
    return nullptr;
  }
  ESP_LOGI(kTag, "active pool: %s", pool_name.c_str());
  // Kick off a one-shot logo fetch in the background. No-op if the
  // cache is already populated. The renderer keeps painting the
  // text-split fallback until the bytes land — see bd btclock_v4-5yi.
  // Fire AFTER the source is built so a later failure-to-spawn doesn't
  // leave the in-flight set populated with no task to drain it.
  const esp_err_t enq =
      btclock::pool_logos::EnqueueFetch(pool_name);
  if (enq != ESP_OK && enq != ESP_ERR_INVALID_STATE) {
    ESP_LOGD(kTag, "logo fetch enqueue for '%s': %s", pool_name.c_str(),
             esp_err_to_name(enq));
  }
  return src;
}

bool PoolSupportsDailyEarnings(const std::string& pool_name) {
  // Empty name: the user hasn't picked a pool yet. Keep the earnings
  // screen available so the settings UI still offers it — the picker
  // default lands on Ocean, which does support earnings.
  if (pool_name.empty()) return true;
  auto src = BuildPoolByName(pool_name);
  if (!src) return true;
  return src->SupportsDailyEarnings();
}

}  // namespace mining_pools
}  // namespace btclock
